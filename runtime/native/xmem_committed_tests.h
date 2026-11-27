void xmem_committed_checks() {
    using namespace ambrosia::services;
    constexpr std::size_t large_page = 2097152;
    MemoryServices memory(large_page * 4);
    const auto allocation = memory.allocate_xmem(1, 0x183, 0x22004, large_page);
    auto *bytes = static_cast<std::byte *>(allocation.address);
    const auto info = memory.xmem_region(bytes);
    check(bytes && !allocation.error && reinterpret_cast<std::uintptr_t>(bytes) % large_page == 0 && info &&
              info->bytes == large_page && info->guest_page_bytes == large_page &&
              info->backing == XmemBacking::private_pages && !memory.window(bytes),
          "committed XMem uses real aligned private backing with logical guest-page rounding");
    check(info->attributes == 0x183 && info->protection == 0x22004 && !memory.xmem_region(bytes + large_page),
          "committed XMem retains complete raw policy and exact region extent");
    check(bytes[0] == std::byte{} && bytes[large_page - 1] == std::byte{} &&
              inspection_page_permissions(bytes).starts_with("rw-"),
          "committed XMem is initially zero-filled CPU read/write memory with no execute permission");
    bytes[0] = std::byte{0x51};
    bytes[large_page - 1] = std::byte{0x62};
    check(bytes[0] == std::byte{0x51} && bytes[large_page - 1] == std::byte{0x62},
          "first and last committed XMem bytes support real independent CPU writes");
    check(memory.free(bytes, 4096, 0x4000).success &&
              memory.allocate(bytes, 4096, 0x1000, 4).address == bytes && bytes[0] == std::byte{} &&
              memory.xmem_region(bytes)->protection == 0x22004,
          "ordinary decommit/recommit discards private pages while retaining the owning XMem policy");
    const auto rounded = memory.allocate_xmem(large_page + 1, 0x283, 0x2404, large_page);
    check(rounded.address && memory.xmem_region(rounded.address)->bytes == large_page * 2 &&
              memory.reserved_bytes() == large_page * 3,
          "committed logical-page rounding is charged to private and aggregate budgets");
    check(memory.free(bytes, 0, 0x8000).success && !memory.xmem_region(bytes) &&
              memory.free(rounded.address, 0, 0x8000).success && memory.reserved_bytes() == 0,
          "committed XMem release removes metadata and returns all private reservation capacity");
    check(memory.allocate_xmem(0, 0x183, 4, large_page).error == error_invalid_parameter &&
              memory.allocate_xmem(std::numeric_limits<std::size_t>::max(), 0x183, 4, large_page).error ==
                  error_invalid_parameter,
          "zero and overflowing committed XMem sizes fail before host allocation");
    check(memory.allocate_xmem(1, 0x183, 4, 8192).error == error_not_supported &&
              memory.allocate_xmem(1, 0x183, 0x20, large_page).error == error_not_supported,
          "unknown logical-page modes and executable committed protections are unsupported");
    MemoryServices bounded(large_page - 1);
    check(bounded.allocate_xmem(1, 0x183, 4, large_page).error == error_not_enough_memory &&
              bounded.reserved_bytes() == 0,
          "rounded committed XMem cannot exceed a private budget even for a one-byte request");

    Runtime runtime(large_page * 2);
    Scope scope(runtime);
    using Allocate =
        void *(AMBROSIA_GUEST_ABI *)(void *, std::size_t, std::uint32_t, std::uint64_t, std::uint32_t);
    using Release = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, std::size_t, std::uint32_t);
    const auto allocate =
        reinterpret_cast<Allocate>(runtime.resolve(service_symbol("XMemVirtualAlloc", "xmem.dll")));
    const auto release =
        reinterpret_cast<Release>(runtime.resolve(service_symbol("VirtualFree", "kernel32.dll")));
    set_last_error(59);
    crt_error() = 37;
    auto *large = allocate(nullptr, 1, 0x20003000u, 0x183, 0x22004);
    check(large && runtime.memory().xmem_region(large)->attributes == 0x183 &&
              runtime.memory().xmem_region(large)->guest_page_bytes == large_page && get_last_error() == 59 &&
              crt_error() == 37,
          "committed five-argument XMem ABI selects logical 2MiB alignment and preserves successful errors");
    auto *small = allocate(nullptr, 1, 0x20403000u, 0x181, 4);
    check(small && runtime.memory().xmem_region(small)->bytes == 65536 &&
              runtime.memory().xmem_region(small)->guest_page_bytes == 65536 &&
              !runtime.memory().window(small),
          "known 64KiB committed XMem profile creates private CPU backing instead of a physical window");
    check(!allocate(nullptr, 1, 0x20003000u, 0x183, 0x40) && get_last_error() == 50 &&
              !allocate(nullptr, large_page * 2, 0x20003000u, 0x183, 0x22004) && get_last_error() == 8 &&
              crt_error() == 37,
          "unsupported committed protection and exhausted private budget fail without disturbing existing "
          "regions");
    check(release(large, 0, 0x8000) == 1 && release(small, 0, 0x8000) == 1 &&
              runtime.memory().reserved_bytes() == 0,
          "committed XMem ABI regions retain normal whole-region VirtualFree ownership");
    check(!allocate(nullptr, 1, 0x20403000u, 0x183, 0x22004) && get_last_error() == 50,
          "committed XMem refuses unobserved combinations of otherwise known page and policy words");
    set_last_error(59);
    auto *commit_only = static_cast<std::byte *>(allocate(nullptr, 1, 0x20401000u, 0x283, 0x42404));
    const auto commit_info = runtime.memory().xmem_region(commit_only);
    check(commit_only && commit_info && commit_info->bytes == 65536 &&
              commit_info->guest_page_bytes == 65536 && commit_info->attributes == 0x283 &&
              commit_info->protection == 0x42404 && commit_info->backing == XmemBacking::private_pages &&
              reinterpret_cast<std::uintptr_t>(commit_only) % 65536 == 0 &&
              !runtime.memory().window(commit_only) && get_last_error() == 59,
          "null-address commit-only XMem creates a real aligned private region with the observed policy");
    if (commit_only) {
        check(commit_only[0] == std::byte{} && commit_only[65535] == std::byte{} &&
                  inspection_page_permissions(commit_only).starts_with("rw-"),
              "commit-only XMem pages start zeroed and readable/writable without execute permission");
        commit_only[0] = std::byte{0x31};
        commit_only[65535] = std::byte{0x72};
        check(allocate(commit_only, 65536, 0x20403000u, 0x283, 0x42404) == nullptr &&
                  get_last_error() == 50 && commit_only[0] == std::byte{0x31} &&
                  commit_only[65535] == std::byte{0x72},
              "fixed-address allocation remains unsupported without changing existing committed bytes");
        check(release(commit_only, 0, 0x8000) == 1 && runtime.memory().reserved_bytes() == 0,
              "commit-only private backing releases through ordinary VirtualFree ownership");
    }
    check(!allocate(nullptr, 1, 0x20401000u, 0x181, 4) && get_last_error() == 50 &&
              !allocate(nullptr, 1, 0x20401000u, 0x283, 0x2404) && get_last_error() == 50,
          "commit-only XMem refuses unobserved combinations of otherwise known attributes/protection");
    check(!allocate(nullptr, 0, 0x20401000u, 0x283, 0x42404) && get_last_error() == 87 &&
              !allocate(nullptr, large_page * 2 + 1, 0x20401000u, 0x283, 0x42404) && get_last_error() == 8 &&
              runtime.memory().reserved_bytes() == 0 && crt_error() == 37,
          "commit-only XMem validates size and private budget before allocating or changing error state");
    std::ostringstream output, errors;
    const std::vector<std::filesystem::path> inspect{"--inspect", "unused", "--boot-memory"};
    check(ambrosia::run_native_command(inspect, output, errors) == 2 &&
              errors.str().find("requires trace-startup") != std::string::npos,
          "larger private boot-memory allowance remains restricted to explicit diagnostic mode");
}
