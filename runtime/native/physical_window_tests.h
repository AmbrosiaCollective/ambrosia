void physical_window_checks(int descriptor) {
    using namespace ambrosia;
    using namespace services;
    constexpr std::size_t page = physical_page_bytes;
    Runtime runtime(page * 8);
    Scope scope(runtime);
    runtime.physical_pages().bind(
        host::SharedBacking::adopt_descriptor(descriptor, PhysicalPages::maximum_backing_bytes));
    auto &physical = runtime.physical_pages();
    const auto capacity = physical.available();
    using Map = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, std::uint64_t, const void *);
    const auto map =
        reinterpret_cast<Map>(runtime.resolve(service_symbol("XMemMapPhysicalPages", "xmem.dll")));
    check(map(nullptr, 0, nullptr) == 1 && map(nullptr, UINT64_MAX, reinterpret_cast<void *>(1)) == 0 &&
              get_last_error() == error_invalid_parameter,
          "empty mappings are harmless and oversized arrays fail before caller data access");
    auto allocation = physical.allocate(0x280, 2);
    check(!allocation.error && allocation.pages.size() == 2,
          "mapping fixture owns two actual backing leases");
    if (allocation.error || allocation.pages.size() != 2) {
        return;
    }
    auto first = runtime.memory().reserve_window(page * 2, 0x27, 0x2404);
    auto second = runtime.memory().reserve_window(page, 0x27, 0x1004);
    auto readonly = runtime.memory().reserve_window(page, 3, 2);
    check(first.address && second.address && readonly.address,
          "physical mapping destinations are owned independent no-access windows");
    if (!first.address || !second.address || !readonly.address) {
        return;
    }
    auto *bytes = static_cast<std::byte *>(first.address);
    auto *alias = static_cast<std::byte *>(second.address);
    std::array<std::byte, 18> packed{};
    packed.fill(std::byte{0xcc});
    std::memcpy(packed.data() + 1, allocation.pages.data(), sizeof(std::uint64_t) * 2);
    const auto saved = packed;
    const auto saved_errno = errno;
    errno = E2BIG;
    set_last_error(89);
    crt_error() = 37;
    const auto mapped = map(first.address, 2, packed.data() + 1);
    check(mapped == 1 && packed == saved && inspection_page_permissions(bytes).starts_with("rw-") &&
              get_last_error() == 89 && errno == E2BIG && crt_error() == 37,
          "packed three-argument mapping preserves inputs/errors and grants only nonexecuting CPU staging");
    if (!mapped) {
        errno = saved_errno;
        return;
    }
    bytes[0] = std::byte{0x31};
    bytes[page] = std::byte{0x72};
    const auto alias_mapped = map(alias, 1, allocation.pages.data());
    check(alias_mapped == 1 && alias[0] == std::byte{0x31},
          "two window addresses read the same physical backing bytes");
    if (!alias_mapped) {
        errno = saved_errno;
        return;
    }
    alias[1] = std::byte{0x55};
    check(bytes[1] == std::byte{0x55}, "physical alias writes are coherent in both directions");
    const auto read_mapped = map(readonly.address, 1, allocation.pages.data());
    check(read_mapped == 1 &&
              inspection_page_permissions(static_cast<std::byte *>(readonly.address)).starts_with("r--") &&
              *static_cast<const std::byte *>(readonly.address) == std::byte{0x31},
          "a read-only window uses the same backing without a writable or executable alias there");
    const std::array<std::uint64_t, 2> bad{allocation.pages[1], UINT64_MAX};
    check(map(bytes, 2, bad.data()) == 0 && get_last_error() == error_invalid_parameter &&
              bytes[0] == std::byte{0x31} && bytes[page] == std::byte{0x72},
          "all tokens validate before any prefix mapping changes");
    check(map(bytes + 1, 1, allocation.pages.data()) == 0 &&
              map(bytes + page, 2, allocation.pages.data()) == 0 &&
              map(reinterpret_cast<void *>(page), 1, allocation.pages.data()) == 0 &&
              bytes[0] == std::byte{0x31},
          "unaligned, out-of-window and foreign destinations cannot replace arbitrary host memory");
    const auto private_region = runtime.memory().allocate(nullptr, page, 0x3000, 4);
    check(private_region.address && map(private_region.address, 1, allocation.pages.data()) == 0 &&
              get_last_error() == error_not_supported,
          "private allocations cannot silently become physical-page windows");
    const auto placeholder = runtime.memory().reserve_placeholder(page * 2, 7);
    check(placeholder.address && runtime.memory().free(placeholder.address, page, 0x8002).success &&
              runtime.memory().replace_placeholder_window(placeholder.address, page, 0x27, 0x8404).address ==
                  placeholder.address &&
              map(placeholder.address, 1, allocation.pages.data()) == 1 &&
              *static_cast<const std::byte *>(placeholder.address) == std::byte{0x31} &&
              map(placeholder.address, 1, nullptr) == 1 &&
              inspection_page_permissions(static_cast<std::byte *>(placeholder.address)).starts_with("---"),
          "an exact replaced child window maps/unmaps leased backing inside its parent's single reservation");
    check(physical.free(std::span(allocation.pages).first(1)) == 0 && physical.available() == capacity - 2 &&
              alias[0] == std::byte{0x31} && bytes[0] == std::byte{0x31},
          "freeing an allocation token retains every live mapped lease and its bytes");
    auto reused = physical.allocate(0x280, 1);
    check(!reused.error && reused.pages.size() == 1 && reused.pages[0] == allocation.pages[0],
          "a retired token ID may be reused while its old mapped backing lease remains distinct");
    if (reused.error || reused.pages.size() != 1) {
        errno = saved_errno;
        return;
    }
    const auto remapped = map(bytes, 1, reused.pages.data());
    check(remapped == 1 && bytes[0] == std::byte{} && alias[0] == std::byte{0x31},
          "remapping a reused token gets fresh storage without changing old aliases");
    if (!remapped) {
        errno = saved_errno;
        return;
    }
    check(map(bytes, 1, nullptr) == 1 && inspection_page_permissions(bytes).starts_with("---") &&
              bytes[page] == std::byte{0x72} && alias[0] == std::byte{0x31},
          "partial unmap restores inaccessible anonymous reservation and preserves adjacent/other aliases");
    const std::uint64_t zero = 0;
    check(map(bytes + page, 1, &zero) == 1 && inspection_page_permissions(bytes + page).starts_with("---"),
          "a zero token explicitly unmaps one page");
    check(map(alias, 1, nullptr) == 1 && physical.available() == capacity - 3,
          "unmapping one old alias cannot release backing retained by another mapped view");
    check(runtime.memory().free(readonly.address, 0, 0x8000).success && physical.available() == capacity - 2,
          "Region destruction removes its mapping before returning the last retired backing lease");
    check(physical.free(reused.pages) == 0 && physical.free(std::span(allocation.pages).subspan(1)) == 0 &&
              physical.available() == capacity,
          "all unmapped backing slots return only after allocation and mapping leases are gone");
    check(runtime.memory().free(first.address, 0, 0x8000).success &&
              runtime.memory().free(second.address, 0, 0x8000).success &&
              runtime.memory().free(private_region.address, 0, 0x8000).success &&
              runtime.memory().reserved_bytes() == page * 2,
          "physical aliases do not duplicate reservation ownership or prevent complete window teardown");
    errno = saved_errno;
}

// The isolated driver adds a deny rule for shared FD3 maps at offset 64 KiB.
// Offset zero remains available, so rollback can restore both original aliases.
void physical_window_rollback_checks(int descriptor) {
    using namespace ambrosia;
    using namespace services;
    constexpr std::size_t page = physical_page_bytes;
    Runtime runtime(page * 4);
    Scope scope(runtime);
    runtime.physical_pages().bind(
        host::SharedBacking::adopt_descriptor(descriptor, PhysicalPages::maximum_backing_bytes));
    auto &physical = runtime.physical_pages();
    const auto capacity = physical.available();
    const auto allocation = physical.allocate(0x280, 2);
    check(!allocation.error && allocation.pages.size() == 2,
          "rollback fixture owns two distinct backing offsets");
    if (allocation.error || allocation.pages.size() != 2) {
        return;
    }
    const auto window = runtime.memory().reserve_window(page * 2, 3, 4);
    using Map = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, std::uint64_t, const void *);
    const auto map =
        reinterpret_cast<Map>(runtime.resolve(service_symbol("XMemMapPhysicalPages", "xmem.dll")));
    const std::array<std::uint64_t, 2> original{allocation.pages[0], allocation.pages[0]};
    const auto initial = window.address ? map(window.address, original.size(), original.data()) : 0;
    check(initial == 1, "the permitted first backing offset maps into both owned destinations");
    if (!initial) {
        return;
    }
    auto *bytes = static_cast<std::byte *>(window.address);
    bytes[0] = std::byte{0x45};
    check(map(bytes, allocation.pages.size(), allocation.pages.data()) == 0 &&
              get_last_error() == error_not_enough_memory && bytes[0] == std::byte{0x45} &&
              bytes[page] == std::byte{0x45} && inspection_page_permissions(bytes + page).starts_with("rw-"),
          "a denied second mapping restores the complete original prefix and failed-page alias");
    bytes[page + 1] = std::byte{0x67};
    check(bytes[1] == std::byte{0x67} && physical.available() == capacity - 2,
          "rollback preserves alias coherence and exact backing ownership");
    check(map(bytes, 2, nullptr) == 1 && physical.free(allocation.pages) == 0 &&
              physical.available() == capacity && runtime.memory().free(bytes, 0, 0x8000).success,
          "rolled-back metadata tears down all mappings and leases without a leak");
}
