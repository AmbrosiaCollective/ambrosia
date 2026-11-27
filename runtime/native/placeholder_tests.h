void placeholder_checks() {
    using namespace ambrosia::services;
    constexpr std::size_t page = 65536;
    MemoryServices memory(MemoryLimits{page, page * 4, page * 5});
    auto parent = memory.reserve_placeholder(page * 3 + 1, 7);
    check(parent.address && !parent.error && memory.reserved_bytes() == page * 4 &&
              reinterpret_cast<std::uintptr_t>(parent.address) % page == 0,
          "placeholder ownership reserves one rounded aligned sparse mapping");
    if (!parent.address) {
        return;
    }
    auto *base = static_cast<std::byte *>(parent.address);
    const auto info = memory.xmem_region(base + page);
    check(info && info->base == base && info->bytes == page * 4 && info->attributes == 7 &&
              info->backing == XmemBacking::placeholder && !memory.window(base),
          "an unsplit placeholder retains metadata without pretending to be a physical window");
    check(inspection_page_permissions(base).starts_with("---") &&
              memory.allocate(base, page, 0x1000, 4).error == error_not_supported &&
              memory.free(base, page, 0x4000).error == error_not_supported,
          "ordinary commit and decommit cannot change placeholder ownership or inaccessible pages");
    const auto ordinary = memory.allocate(nullptr, page, 0x3000, 4);
    check(ordinary.address && memory.reserved_bytes() == page * 5 &&
              memory.reserve_placeholder(1, 7).error == error_not_enough_memory,
          "placeholders use the sparse budget while private allocations retain their own limit");
    check(memory.free(base, page, 0x8002).success && memory.free(base + page, page, 0x8002).success &&
              memory.free(base + page * 2, page, 0x8002).success && memory.reserved_bytes() == page * 5,
          "explicit placeholder splits retain a single mapping and unchanged reservation charge");
    const auto first = memory.replace_placeholder_window(base + page, 1, 3, 0x801);
    const auto last = memory.replace_placeholder_window(base + page * 3, page, 0x23, 0x2404);
    const auto child = memory.window(base + page);
    check(first.address == base + page && last.address == base + page * 3 && child &&
              child->base == base + page && child->bytes == page && child->attributes == 3 &&
              child->protection == 0x801 && memory.reserved_bytes() == page * 5,
          "replacement windows retain policy and share the existing reservation without double charging");
    const auto last_info = memory.window(base + page * 3);
    check(memory.window(base + page * 2 - 1) && !memory.window(base + page * 2) && last_info &&
              last_info->attributes == 0x23 && inspection_page_permissions(base + page).starts_with("---"),
          "child bounds exclude placeholder gaps and no physical pages are fabricated");
    const auto repeated = memory.replace_placeholder_window(base + page, page, 7, 4);
    const auto overlap = memory.replace_placeholder_window(base + page * 2, page * 2, 7, 4);
    const auto preserved = memory.window(base + page);
    check(repeated.error == error_invalid_address && overlap.error == error_invalid_address && preserved &&
              preserved->attributes == 3 && memory.reserved_bytes() == page * 5,
          "overlapping replacements fail atomically and retain the previous children and accounting");
    check(memory.replace_placeholder_window(base + 1, page, 3, 4).error == error_invalid_parameter &&
              memory.replace_placeholder_window(base + page * 3, page * 2, 3, 4).error ==
                  error_invalid_address &&
              memory.replace_placeholder_window(ordinary.address, page, 3, 4).error == error_not_supported,
          "unaligned, out-of-parent and nonplaceholder replacements cannot overwrite other memory");
    check(memory.free(base, 0, 0x8000).error == error_not_supported &&
              memory.free(base + page, 0, 0x8000).error == error_invalid_address &&
              memory.window(base + page),
          "unsupported populated-parent and child release conventions preserve their sole owner");
    check(memory.free(ordinary.address, 0, 0x8000).success && memory.reserved_bytes() == page * 4,
          "private release reclaims only its own accounting beside a populated placeholder");
    MemoryServices bare(page * 2);
    const auto empty = bare.reserve_placeholder(page, 7);
    check(empty.address && bare.free(empty.address, 0, 0x8000).success && !bare.xmem_region(empty.address) &&
              !bare.reserved_bytes(),
          "an unpopulated placeholder releases its whole mapping and metadata normally");
    check(bare.reserve_placeholder(0, 7).error == error_invalid_parameter &&
              bare.reserve_placeholder(SIZE_MAX, 7).error == error_invalid_parameter &&
              bare.replace_placeholder_window(nullptr, page, 3, 4).error == error_invalid_parameter,
          "zero and overflowing placeholder sizes fail before any reservation");
    MemoryServices capacity(page * 65);
    const auto large = capacity.reserve_placeholder(page * 65, 7);
    bool accepted = large.address;
    for (std::size_t index = 0; accepted && index < 64; ++index) {
        auto *address = static_cast<std::byte *>(large.address) + index * page;
        accepted = capacity.free(address, page, 0x8002).success &&
                   capacity.replace_placeholder_window(address, page, 3, 4).error == 0;
    }
    check(accepted &&
              capacity.replace_placeholder_window(static_cast<std::byte *>(large.address) + page * 64, page,
                                                  3, 4)
                      .error == error_not_enough_memory &&
              capacity.reserved_bytes() == page * 65,
          "the bounded child count rejects excess metadata without allocating more virtual space");

    Runtime runtime(page * 5);
    Scope scope(runtime);
    using Allocate =
        void *(AMBROSIA_GUEST_ABI *)(void *, std::size_t, std::uint32_t, std::uint64_t, std::uint32_t);
    const auto allocate =
        reinterpret_cast<Allocate>(runtime.resolve(service_symbol("XMemVirtualAlloc", "xmem.dll")));
    const auto saved_errno = errno;
    errno = E2BIG;
    set_last_error(89);
    crt_error() = 37;
    auto *reserved = static_cast<std::byte *>(allocate(nullptr, page * 3, 0x42000, 7, 1));
    const auto reserved_info = runtime.memory().xmem_region(reserved);
    check(reserved && reserved_info && reserved_info->backing == XmemBacking::placeholder &&
              get_last_error() == 89 && errno == E2BIG && crt_error() == 37,
          "the five-argument XMem ABI preserves no-access reservation metadata and error channels");
    if (reserved) {
        check(runtime.memory().free(reserved, page, 0x8002).success &&
                  runtime.memory().free(reserved + page, page, 0x8002).success,
              "ABI replacement uses explicit prior split boundaries");
        const auto replacement = allocate(reserved + page, page, 0x20406000, 0x23, 0x2404);
        const auto replacement_info = runtime.memory().window(reserved + page);
        check(replacement == reserved + page && replacement_info && replacement_info->protection == 0x2404 &&
                  get_last_error() == 89 && runtime.memory().reserved_bytes() == page * 3,
              "known XMem replacement profiles reach their existing parent without another mapping");
        check(!allocate(reserved + page, page, 0x20406000, 3, 4) &&
                  get_last_error() == error_invalid_address && runtime.memory().window(reserved + page),
              "ABI replacement overlap reports an error without dropping an existing window");
        const auto inaccessible = allocate(reserved, page, 0x20406000, 7, 1);
        const auto inaccessible_info = runtime.memory().window(reserved);
        check(inaccessible == reserved && inaccessible_info && inaccessible_info->protection == 1 &&
                  inspection_page_permissions(reserved).starts_with("---"),
              "PAGE_NOACCESS replacement metadata grants no CPU access or physical backing");
    }
    check(!allocate(nullptr, page, 0x42000, 0xdead, 1) && get_last_error() == error_not_supported &&
              !allocate(nullptr, page, 0x42000, 7, 0x40) && get_last_error() == error_not_supported &&
              !allocate(nullptr, page, 0x20406000, 3, 4) && get_last_error() == error_invalid_parameter &&
              errno == E2BIG && crt_error() == 37,
          "unknown/executable profiles and missing replacement addresses fail without invented storage");
    errno = saved_errno;
}

void placeholder_split_checks() {
    using namespace ambrosia::services;
    constexpr std::size_t page = 65536;
    MemoryServices memory(page * 8);
    const auto parent = memory.reserve_placeholder(page * 4, 7);
    check(parent.address, "placeholder split fixture owns its parent");
    if (!parent.address) {
        return;
    }
    auto *base = static_cast<std::byte *>(parent.address);
    check(memory.replace_placeholder_window(base + page, page, 0x27, 0x1004).error == error_invalid_address,
          "a subrange cannot replace a placeholder before explicit split boundaries exist");
    check(memory.free(base + page, page, 0x8002).error == error_invalid_address &&
              memory.free(base, page + 1, 0x8002).error == error_invalid_parameter &&
              memory.free(base + 1, page, 0x8002).error == error_invalid_parameter &&
              memory.free(base, 0, 0x8002).error == error_invalid_parameter &&
              memory.reserved_bytes() == page * 4,
          "unaligned, empty and interior three-way split requests leave ownership intact");
    check(memory.free(base + page * 3, page, 0x8002).success && memory.free(base, page, 0x8002).success &&
              memory.replace_placeholder_window(base, page, 0x27, 0x1004).address == base &&
              memory.replace_placeholder_window(base + page * 3, page, 0x27, 0x8404).address ==
                  base + page * 3,
          "prefix and suffix splits produce exact replaceable segments without changing the mapping");
    check(memory.free(base, page, 0x8002).error == error_invalid_address &&
              memory.replace_placeholder_window(base + page, page, 0x27, 0x2404).error ==
                  error_invalid_address &&
              memory.free(base, 0, 0x8000).error == error_not_supported &&
              inspection_page_permissions(base).starts_with("---") && memory.reserved_bytes() == page * 4,
          "replaced and still-unsplit segments reject unsupported changes and remain inaccessible");
    Runtime runtime(page * 8);
    Scope scope(runtime);
    using Allocate =
        void *(AMBROSIA_GUEST_ABI *)(void *, std::size_t, std::uint32_t, std::uint64_t, std::uint32_t);
    using Free = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, std::size_t, std::uint32_t);
    const auto allocate =
        reinterpret_cast<Allocate>(runtime.resolve(service_symbol("XMemVirtualAlloc", "xmem.dll")));
    const auto release =
        reinterpret_cast<Free>(runtime.resolve(service_symbol("VirtualFree", "kernel32.dll")));
    auto *reserved = static_cast<std::byte *>(allocate(nullptr, page * 6, 0x42000, 7, 1));
    check(reserved, "split ABI fixture owns one no-access reservation");
    if (reserved) {
        const std::array<std::uint32_t, 5> protections{0x1004, 0x1404, 0x8404, 0x2404, 0x2004};
        bool valid = true;
        set_last_error(89);
        crt_error() = 37;
        const auto saved_errno = errno;
        errno = E2BIG;
        for (std::size_t index = 0; index < protections.size(); ++index) {
            auto *address = reserved + index * page;
            valid = release(address, page, 0x8002) == 1 && valid;
            valid = allocate(address, page, 0x20406000, 0x27, protections[index]) == address && valid;
            const auto window = runtime.memory().window(address);
            valid = window && window->protection == protections[index] && window->attributes == 0x27 && valid;
        }
        check(valid && runtime.memory().reserved_bytes() == page * 6 && get_last_error() == 89 &&
                  crt_error() == 37 && errno == E2BIG,
              "all documented reserve-only replacement profiles retain exact policy through the ABI");
        errno = saved_errno;
    }
    MemoryServices capacity(page * 129);
    const auto many = capacity.reserve_placeholder(page * 129, 7);
    bool complete = many.address;
    for (std::size_t index = 0; complete && index < 127; ++index) {
        complete = capacity.free(static_cast<std::byte *>(many.address) + index * page, page, 0x8002).success;
    }
    check(complete &&
              capacity.free(static_cast<std::byte *>(many.address) + page * 127, page, 0x8002).error ==
                  error_not_enough_memory &&
              capacity.reserved_bytes() == page * 129,
          "placeholder segment count is bounded independently from reservation bytes and child count");
}

void placeholder_thread_checks() {
    using namespace ambrosia::services;
    constexpr std::size_t page = 65536;
    MemoryServices memory(page * 3);
    const auto parent = memory.reserve_placeholder(page * 3, 7);
    check(parent.address, "concurrent replacement fixture owns a sparse reservation");
    if (!parent.address) {
        return;
    }
    auto *child = static_cast<std::byte *>(parent.address) + page;
    check(memory.free(parent.address, page, 0x8002).success && memory.free(child, page, 0x8002).success,
          "the racing replacement range has exact explicit placeholder boundaries");
    std::array<AllocationResult, 2> results{};
    std::barrier gate(3);
    auto work = [&](std::size_t index) {
        gate.arrive_and_wait();
        results[index] = memory.replace_placeholder_window(child, page, 3, 4);
    };
    std::thread first(work, 0);
    std::thread second;
    try {
        second = std::thread(work, 1);
    } catch (...) {
        gate.arrive_and_drop();
        gate.arrive_and_wait();
        first.join();
        throw;
    }
    gate.arrive_and_wait();
    first.join();
    second.join();
    const auto accepted = (results[0].error == 0) + (results[1].error == 0);
    const auto rejected =
        (results[0].error == error_invalid_address) + (results[1].error == error_invalid_address);
    const auto info = memory.window(child);
    check(accepted == 1 && rejected == 1 && memory.reserved_bytes() == page * 3 && info &&
              info->bytes == page,
          "competing replacements serialize to one owner and one unchanged reservation charge");
}
