// Included after the native harness helpers; no child threads are created.
void thread_checks() {
    using ambrosia::guest::ThreadContext;
    using ambrosia::guest::TlsTemplate;
    ThreadContext first, second;
    TlsTemplate spec{{std::byte{42}, std::byte{7}}, 14, 64};
    first.install(0, spec);
    first.install(3, spec);
    second.install(0, spec);
    auto block = first.block(0);
    check(block.size() == 16 && block[0] == std::byte{42} && block[1] == std::byte{7},
          "TLS initial bytes copied");
    check(std::all_of(block.begin() + 2, block.end(), [](auto b) { return b == std::byte{0}; }),
          "TLS tail is zero-filled");
    check(reinterpret_cast<std::uintptr_t>(block.data()) % 64 == 0, "TLS block alignment respected");
    first.block(0)[0] = std::byte{19};
    check(second.block(0)[0] == std::byte{42} && first.block(3)[0] == std::byte{42},
          "TLS contexts and slots have independent storage");
    bool duplicate = false;
    try {
        first.install(0, spec);
    } catch (const std::runtime_error &) {
        duplicate = true;
    }
    check(duplicate, "duplicate TLS slot rejected");
    bool invalid = false;
    try {
        first.install(64, spec);
    } catch (const std::runtime_error &) {
        invalid = true;
    }
    check(invalid, "out-of-range TLS slot rejected");
    bool bad_alignment = false;
    try {
        auto bad = spec;
        bad.alignment = 3;
        first.install(1, bad);
    } catch (const std::runtime_error &) {
        bad_alignment = true;
    }
    check(bad_alignment, "non-power-of-two TLS alignment rejected");
    bool oversized = false;
    try {
        auto bad = spec;
        bad.zero_fill = 1024 * 1024;
        first.install(1, bad);
    } catch (const std::runtime_error &) {
        oversized = true;
    }
    check(oversized, "oversized TLS storage rejected");
#if defined(__linux__) && defined(__x86_64__)
    const auto before = ambrosia::host::guest_segment_base();
    static thread_local unsigned native_tls = 123;
    native_tls = 123;
    {
        auto scope = first.activate();
        const auto outer = ambrosia::host::guest_segment_base();
        void **slots = nullptr;
        __asm__ volatile("movq %%gs:0x58, %0" : "=r"(slots));
        check(slots[0] == first.block(0).data() && slots[3] == first.block(3).data(),
              "GS anchor exposes guest TLS slots");
        check(native_tls == 123, "native C++ TLS remains available in guest scope");
        native_tls = 456;
        try {
            auto inner = second.activate();
            check(ambrosia::host::guest_segment_base() != outer, "nested context installs distinct GS base");
            throw std::runtime_error("host-only unwind probe");
        } catch (const std::runtime_error &) {
        }
        check(ambrosia::host::guest_segment_base() == outer, "nested scope restores GS on host unwind");
    }
    check(ambrosia::host::guest_segment_base() == before && native_tls == 456,
          "scope restores original GS without changing native TLS");
    bool sealed = false;
    try {
        first.install(1, spec);
    } catch (const std::runtime_error &) {
        sealed = true;
    }
    check(sealed, "TLS layout sealed after activation");
#else
    bool unsupported = false;
    try {
        auto scope = first.activate();
    } catch (const std::runtime_error &) {
        unsupported = true;
    }
    check(unsupported, "unsupported guest thread adapter fails explicitly");
#endif
}
