void xmem_checks() {
    using namespace ambrosia::services;
    constexpr std::size_t guest_page = 65536;
    MemoryServices memory(guest_page * 3);
    const auto first = memory.reserve_window(1, 3, 0x801);
    check(first.address && !first.error &&
              reinterpret_cast<std::uintptr_t>(first.address) % guest_page == 0 &&
              memory.reserved_bytes() == guest_page,
          "XMem window reserves real aligned address space rounded to its guest page size");
    const auto info = memory.window(first.address);
    check(info && info->base == first.address && info->bytes == guest_page && info->attributes == 3 &&
              info->protection == 0x801 &&
              memory.window(static_cast<std::byte *>(first.address) + guest_page - 1) &&
              !memory.window(static_cast<std::byte *>(first.address) + guest_page),
          "owned window retains raw mapping policy and enforces its exact address extent");
    check(inspection_page_permissions(static_cast<std::byte *>(first.address)).starts_with("---"),
          "reserved XMem window is genuinely inaccessible and non-executable before physical mapping");
    check(memory.allocate(first.address, guest_page, 0x1000, 4).error == error_not_supported &&
              memory.free(first.address, guest_page, 0x4000).error == error_not_supported &&
              inspection_page_permissions(static_cast<std::byte *>(first.address)).starts_with("---"),
          "ordinary commit/decommit cannot silently replace physical-window backing semantics");
    const auto second = memory.reserve_window(guest_page + 1, 0x21, 4);
    check(second.address && memory.window(second.address)->bytes == guest_page * 2 &&
              memory.reserved_bytes() == guest_page * 3,
          "window rounding and shared reservation accounting include every owned guest page");
    check(memory.reserve_window(1, 0x23, 0x2004).error == error_not_enough_memory &&
              memory.window(first.address) && memory.window(second.address),
          "exhausted window budget refuses new memory while preserving existing ownership");
    check(memory.free(static_cast<std::byte *>(first.address) + 1, 0, 0x8000).error ==
                  error_invalid_address &&
              memory.free(first.address, 1, 0x8000).error == error_invalid_parameter,
          "window release requires its exact owned base and whole-region release convention");
    check(memory.free(first.address, 0, 0x8000).success && !memory.window(first.address) &&
              memory.reserved_bytes() == guest_page * 2,
          "whole-window release removes both real reservation and raw-policy metadata");
    check(memory.free(second.address, 0, 0x8000).success && memory.reserved_bytes() == 0,
          "all window ownership and budget are reclaimed on release");
    check(memory.reserve_window(0, 3, 0x801).error == error_invalid_parameter &&
              memory.reserve_window(std::numeric_limits<std::size_t>::max(), 3, 0x801).error ==
                  error_invalid_parameter &&
              memory.reserved_bytes() == 0,
          "zero and rounding-overflow window requests fail before reservation");
    check(memory.reserve_window(24ull << 30, 3, 0x801).error == error_not_enough_memory &&
              memory.reserved_bytes() == 0,
          "oversized synthetic window is rejected without a huge mapping or weaker sandbox");
    const auto ordinary = memory.allocate(nullptr, 4096, 0x3000, 4);
    check(ordinary.address && !memory.window(ordinary.address),
          "ordinary private allocations do not acquire physical-window metadata");

    Runtime runtime(guest_page * 2);
    Scope scope(runtime);
    using Allocate =
        void *(AMBROSIA_GUEST_ABI *)(void *, std::size_t, std::uint32_t, std::uint64_t, std::uint32_t);
    using Release = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, std::size_t, std::uint32_t);
    const auto allocate =
        reinterpret_cast<Allocate>(runtime.resolve(service_symbol("XMemVirtualAlloc", "xmem.dll")));
    const auto release =
        reinterpret_cast<Release>(runtime.resolve(service_symbol("VirtualFree", "kernel32.dll")));
    set_last_error(83);
    crt_error() = 31;
    auto *window = allocate(nullptr, 1, 0x20402000u, 3, 0x801);
    check(window && runtime.memory().window(window)->protection == 0x801 && get_last_error() == 83 &&
              crt_error() == 31,
          "five-argument Microsoft ABI receives stack protection and preserves successful error channels");
    check(!allocate(nullptr, 24ull << 30, 0x20402000u, 3, 0x801) && get_last_error() == 8 &&
              runtime.memory().window(window) && crt_error() == 31,
          "oversized XMem ABI request fails out-of-memory without leaking or fabricating a pointer");
    check(!allocate(nullptr, 0, 0x20402000u, 3, 0x801) && get_last_error() == 87,
          "zero-size XMem ABI reservation reports invalid-parameter");
    check(!allocate(window, 1, 0x20402000u, 3, 0x801) && get_last_error() == 50 &&
              !allocate(nullptr, 1, 0x20403000u, 3, 4) && get_last_error() == 50,
          "fixed-address and unsupported committed XMem profiles do not overwrite a reservation");
    check(!allocate(nullptr, 1, 0x20402000u, 0xdead, 4) && get_last_error() == 50 &&
              !allocate(nullptr, 1, 0x20402000u, 3, 0x40) && get_last_error() == 50,
          "unknown attributes and executable protections are refused instead of silently stripped");
    auto *alias_profile = allocate(nullptr, 1, 0x20402000u, 0x23, 0x2404);
    check(alias_profile && runtime.memory().window(alias_profile)->attributes == 0x23 &&
              runtime.memory().window(alias_profile)->protection == 0x2404,
          "known reserve profile retains complete GPU/cache policy without claiming physical backing");
    check(release(window, 0, 0x8000) == 1 && release(alias_profile, 0, 0x8000) == 1 &&
              runtime.memory().reserved_bytes() == 0 && crt_error() == 31,
          "existing VirtualFree ABI releases owned XMem windows through the shared memory owner");
}
