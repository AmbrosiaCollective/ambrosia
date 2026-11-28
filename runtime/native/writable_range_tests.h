void writable_range_checks(int descriptor) {
    using namespace ambrosia;
    using namespace services;
    constexpr auto page = physical_page_bytes;
    PhysicalPages physical;
    physical.bind(host::SharedBacking::adopt_descriptor(descriptor, PhysicalPages::maximum_backing_bytes));
    MemoryServices memory(page * 8);
    const auto ordinary = memory.allocate(nullptr, 12288, 0x2000, 1);
    auto *base = static_cast<std::byte *>(ordinary.address);
    check(base && !memory.writable_range(base, 1),
          "reserved private storage is not writable descriptor memory");
    if (!base) {
        return;
    }
    check(memory.allocate(base, 4096, 0x1000, 4).address &&
              memory.allocate(base + 8192, 4096, 0x1000, 4).address &&
              memory.writable_range(base + 1, 4095) && !memory.writable_range(base, 12288),
          "private byte ranges validate every committed page rather than their first page only");
    check(memory.allocate(base + 4096, 4096, 0x1000, 2).address && !memory.writable_range(base + 4095, 2) &&
              !memory.writable_range(base + 4096, 1),
          "read-only pages cannot be crossed by a writable descriptor extent");
    check(!memory.writable_range(nullptr, 1) && !memory.writable_range(base, 0) &&
              !memory.writable_range(reinterpret_cast<void *>(UINTPTR_MAX - 3), 8) &&
              !memory.writable_range(base, 64 * 1024 * 1024 + 1),
          "null, empty, overflowing and oversized descriptor extents fail before access");
    const auto allocation = physical.allocate(0x280, 2);
    if (allocation.error || allocation.pages.size() != 2) {
        throw std::runtime_error("writable range fixture cannot allocate physical pages");
    }
    std::array<std::shared_ptr<const PhysicalPage>, 2> leases{physical.lease(allocation.pages[0]),
                                                              physical.lease(allocation.pages[1])};
    auto *window = static_cast<std::byte *>(memory.reserve_window(page * 2, 0x27, 0x1004).address);
    auto *readonly = static_cast<std::byte *>(memory.reserve_window(page, 3, 2).address);
    if (!window || !readonly) {
        throw std::runtime_error("writable range fixture cannot reserve windows");
    }
    check(!memory.writable_range(window, 1) && memory.map_physical(window, leases) == 0 &&
              memory.writable_range(window + 1, page) &&
              memory.map_physical(readonly, std::span(leases).first(1)) == 0 &&
              !memory.writable_range(readonly, 1),
          "physical windows require mapped leases and writable policy across every page");
    const std::array<std::shared_ptr<const PhysicalPage>, 1> empty{};
    check(memory.map_physical(window + page, empty) == 0 && memory.writable_range(window, page) &&
              !memory.writable_range(window, page + 1),
          "unmapping a physical page invalidates a crossing writable range immediately");
}
