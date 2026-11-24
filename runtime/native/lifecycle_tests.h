struct LifecycleEvent {
    char kind;
    std::uint32_t reason;
    std::uint32_t value;
    std::uint32_t marker;
};

std::vector<LifecycleEvent> lifecycle_events;
const std::byte *expected_module_base = nullptr;
bool lifecycle_arguments_valid = true;
bool reject_dll_attach = false;

std::uint32_t *current_guest_tls() {
#if defined(__linux__) && defined(__x86_64__)
    void **slots = nullptr;
    __asm__ volatile("movq %%gs:0x58, %0" : "=r"(slots));
    return static_cast<std::uint32_t *>(slots[0]);
#else
    return nullptr;
#endif
}

void AMBROSIA_GUEST_ABI record_tls_callback(void *module, std::uint32_t reason, void *reserved) {
    auto *tls = current_guest_tls();
    lifecycle_arguments_valid = lifecycle_arguments_valid && module == expected_module_base && !reserved;
    lifecycle_events.push_back({'T', reason, tls[0], tls[1]});
    tls[1] = reason == 1 ? 0x33 : 0x44;
}

std::int32_t AMBROSIA_GUEST_ABI record_dll_entry(void *module, std::uint32_t reason, void *reserved) {
    auto *tls = current_guest_tls();
    lifecycle_arguments_valid = lifecycle_arguments_valid && module == expected_module_base && !reserved;
    lifecycle_events.push_back({'D', reason, tls[0], tls[1]});
    return !(reject_dll_attach && reason == 1);
}

void jump_to_host(Bytes &bytes, std::size_t offset, std::uintptr_t address) {
    const unsigned char jump[] = {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0};
    std::memcpy(bytes.data() + offset, jump, sizeof(jump));
    put<std::uint64_t>(bytes, offset + 2, address);
}

Bytes tls_fixture(bool dll = false) {
    auto bytes = fixture();
    constexpr std::uint64_t preferred = 0x180000000ull;
    put<std::uint32_t>(bytes, 0x98 + 184, 0x2120);
    put<std::uint32_t>(bytes, 0x98 + 188, 40);
    put<std::uint64_t>(bytes, 0x520, preferred + 0x2160);
    put<std::uint64_t>(bytes, 0x528, preferred + 0x2164);
    put<std::uint64_t>(bytes, 0x530, preferred + 0x2170);
    put<std::uint64_t>(bytes, 0x538, preferred + 0x2180);
    put<std::uint32_t>(bytes, 0x540, 12);
    put<std::uint32_t>(bytes, 0x544, 7 << 20); // 64-byte alignment
    put<std::uint32_t>(bytes, 0x560, 41);
    put<std::uint32_t>(bytes, 0x570, 0xffffffff);
    put<std::uint64_t>(bytes, 0x580, preferred + 0x1100);
    jump_to_host(bytes, 0x300, reinterpret_cast<std::uintptr_t>(&record_tls_callback));
    const std::array<std::uint16_t, 6> relocations{0xa100, 0xa120, 0xa128, 0xa130, 0xa138, 0xa180};
    put<std::uint32_t>(bytes, 0x604, 20);
    put<std::uint32_t>(bytes, 0x98 + 156, 20);
    for (std::size_t index = 0; index < relocations.size(); ++index) {
        put<std::uint16_t>(bytes, 0x608 + index * 2, relocations[index]);
    }
    if (dll) {
        put<std::uint16_t>(bytes, 0x96, 0x2022);
        jump_to_host(bytes, 0x200, reinterpret_cast<std::uintptr_t>(&record_dll_entry));
    } else {
        // Read slot zero through the guest TLS anchor, increment its first
        // 32-bit value, and return it. No host import or game code involved.
        const unsigned char entry[] = {0x65, 0x48, 0x8b, 0x04, 0x25, 0x58, 0,    0, 0,
                                       0x48, 0x8b, 0,    0x83, 0,    1,    0x8b, 0, 0xc3};
        std::memcpy(bytes.data() + 0x200, entry, sizeof(entry));
    }
    return bytes;
}

template <class Mutation> void bad_tls(Mutation mutation, const char *label) {
    auto bytes = tls_fixture();
    mutation(bytes);
    check(rejected(bytes), label);
}

void lifecycle_checks() {
    bad_tls([](auto &b) { put<std::uint32_t>(b, 0x98 + 184, 0x3f00); },
            "TLS directory outside declared sections rejected");
    bad_tls([](auto &b) { put<std::uint32_t>(b, 0x98 + 188, 39); }, "short TLS directory rejected");
    bad_tls([](auto &b) { put<std::uint64_t>(b, 0x528, 0x18000215full); }, "reversed TLS template rejected");
    bad_tls([](auto &b) { put<std::uint32_t>(b, 0x540, 1024 * 1024); }, "TLS zero-fill overflow rejected");
    bad_tls([](auto &b) { put<std::uint32_t>(b, 0x544, 15 << 20); }, "reserved TLS alignment rejected");
    bad_tls([](auto &b) { put<std::uint32_t>(b, 0x544, 1); }, "unknown TLS flags rejected");
    bad_tls([](auto &b) { put<std::uint64_t>(b, 0x530, 0x180001000ull); }, "read-only TLS index rejected");
    bad_tls([](auto &b) { put<std::uint64_t>(b, 0x530, 0x180002171ull); }, "misaligned TLS index rejected");
    bad_tls([](auto &b) { put<std::uint64_t>(b, 0x580, 0x180002160ull); },
            "non-executable TLS callback rejected");
    bad_tls([](auto &b) { put<std::uint64_t>(b, 0x538, 0x180004000ull); },
            "out-of-image callback table rejected");
    bad_tls([](auto &b) { put<std::uint64_t>(b, 0x528, 0x180003000ull); },
            "TLS template crossing section boundary rejected");
#if defined(__linux__) && defined(__x86_64__)
    lifecycle_events.clear();
    const auto original_gs = ambrosia::host::guest_segment_base();
    auto image = ambrosia::pe::Image::load(tls_fixture(), resolve);
    expected_module_base = image.base();
    check(lifecycle_events.empty(), "mapping TLS does not execute callbacks");
    std::uint32_t index = 99;
    std::memcpy(&index, image.base() + 0x2170, sizeof(index));
    check(index == 0, "module TLS index assigned before initialization");
    auto execution = image.start();
    check(lifecycle_events.size() == 1 && lifecycle_events[0].reason == 1 &&
              lifecycle_events[0].value == 41 && lifecycle_events[0].marker == 0,
          "TLS process attach sees initialized and zero-filled storage");
    check(execution.run_bootstrap() == 42 && execution.run_bootstrap() == 43,
          "guest TLS survives calls in one execution session");
    check(ambrosia::host::guest_segment_base() == original_gs, "entry returns with original host GS");
    bool duplicate = false;
    try {
        auto other = image.start();
    } catch (const std::runtime_error &) {
        duplicate = true;
    }
    check(duplicate && execution.run_bootstrap() == 44, "duplicate session cannot disturb active module");
    execution.close();
    check(lifecycle_events.size() == 2 && lifecycle_events[1].reason == 0 && lifecycle_events[1].value == 44,
          "TLS process detach sees final session values");
    execution.close();
    check(lifecycle_events.size() == 2, "explicit close is idempotent");
    bool closed = false;
    try {
        (void)execution.run_bootstrap();
    } catch (const std::runtime_error &) {
        closed = true;
    }
    check(closed, "closed session cannot run guest code");
    {
        auto next = image.start();
        check(next.run_bootstrap() == 42, "new session starts from fresh TLS template");
    }
    {
        auto retained = [&] {
            auto temporary = ambrosia::pe::Image::load(tls_fixture(), resolve);
            expected_module_base = temporary.base();
            return temporary.start();
        }();
        check(retained.run_bootstrap() == 42, "execution retains mapping after image handle destruction");
    }

    lifecycle_events.clear();
    auto dll = ambrosia::pe::Image::load(tls_fixture(true), resolve);
    expected_module_base = dll.base();
    {
        auto loaded = dll.start();
        check(lifecycle_events.size() == 2 && lifecycle_events[0].kind == 'T' &&
                  lifecycle_events[1].kind == 'D' && lifecycle_events[1].reason == 1 &&
                  lifecycle_events[1].marker == 0x33,
              "TLS callback precedes dynamic DLL process attach");
        bool wrong_entry = false;
        try {
            (void)loaded.run_bootstrap();
        } catch (const std::runtime_error &) {
            wrong_entry = true;
        }
        check(wrong_entry, "DLL entry cannot be called with executable ABI");
    }
    check(lifecycle_events.size() == 4 && lifecycle_events[2].kind == 'T' &&
              lifecycle_events[3].kind == 'D' && lifecycle_events[3].reason == 0 &&
              lifecycle_events[3].marker == 0x44,
          "TLS and DLL detach finish before unmapping");
    lifecycle_events.clear();
    reject_dll_attach = true;
    bool rejected_attach = false;
    try {
        auto loaded = dll.start();
    } catch (const std::runtime_error &) {
        rejected_attach = true;
    }
    reject_dll_attach = false;
    check(rejected_attach && lifecycle_events.size() == 4, "failed DLL attach performs detach cleanup");
    {
        auto retry = dll.start();
        check(lifecycle_events.size() == 6, "failed attach releases module execution lease");
    }
    check(lifecycle_arguments_valid,
          "lifecycle callbacks receive module, reason and dynamic reserved argument");
    check(ambrosia::host::guest_segment_base() == original_gs, "lifecycle failure cleanup restores host GS");
#endif
}
