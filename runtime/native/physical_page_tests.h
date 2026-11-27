void physical_page_checks(int descriptor) {
    using namespace ambrosia;
    using namespace services;
    std::optional<PhysicalPages> owner;
    owner.emplace();
    auto &pages = *owner;
    check(pages.allocate(0x280, 1).error == error_not_supported && pages.allocate(0x280, 0).error == 0,
          "nonempty physical allocations require owned backing while an empty request owns no pages");
    pages.bind(host::SharedBacking::adopt_descriptor(descriptor, PhysicalPages::maximum_backing_bytes));
    const auto capacity = pages.capacity();
    check(capacity == 16384 && pages.available() == capacity,
          "the immutable1GiB backing bounds the allocator to16384 guest-sized pages");
    check(pages.allocate(0x281, 1).error == error_not_supported && pages.available() == capacity,
          "unmeasured attribute words cannot allocate pages or alter the budget");
    auto first = pages.allocate(0x280, 2);
    check(!first.error && first.pages.size() == 2 && first.pages[0] && first.pages[0] != first.pages[1] &&
              first.pages[0] % 16 == 0 && first.pages[1] < (std::uint64_t{1} << 25),
          "each physical token names a distinct owned64KiB lease in the inherited packed frame-ID range");
    if (first.error || first.pages.size() != 2) {
        return;
    }
    auto held = pages.lease(first.pages[0]);
    if (!held) {
        check(false, "an allocated physical ID resolves to its live backing lease");
        return;
    }
    host::Mapping retired(physical_page_bytes, host::Access::none, physical_page_bytes);
    host::Mapping current(physical_page_bytes, host::Access::none, physical_page_bytes);
    held->map_into(retired, 0, host::Access::read_write);
    retired.data()[3] = std::byte{0x41};
    const auto freed = pages.free(std::span(first.pages).first(1));
    check(
        !freed && !pages.lease(first.pages[0]) && pages.available() == capacity - 2 &&
            retired.data()[3] == std::byte{0x41},
        "freeing an ID retires its lookup but preserves the bytes and budget of an outstanding alias lease");
    const auto retained_byte = retired.data()[3];
    auto replacement = pages.allocate(0x280, 1);
    check(!replacement.error && replacement.pages.size() == 1 && replacement.pages[0] == first.pages[0],
          "a freed token can be reused without reusing its still-leased backing bytes");
    if (replacement.error || replacement.pages.size() != 1) {
        return;
    }
    auto live = pages.lease(replacement.pages[0]);
    if (!live) {
        check(false, "a replacement ID resolves to its new backing lease");
        return;
    }
    live->map_into(current, 0, host::Access::read_write);
    const bool initially_zero = current.data()[3] == std::byte{0};
    current.data()[3] = std::byte{0xbb};
    check(initially_zero && retired.data()[3] == retained_byte,
          "reused frame IDs cannot alias an earlier retired lease still held by another view");
    retired.decommit(0, physical_page_bytes);
    held.reset();
    check(pages.available() == capacity - 2,
          "retiring the final alias returns its backing page to the available budget");
    auto reused_storage = pages.allocate(0x280, 1);
    auto reused_lease = reused_storage.pages.empty() ? nullptr : pages.lease(reused_storage.pages[0]);
    if (!reused_lease) {
        check(false, "reclaimed backing remains allocatable");
        return;
    }
    reused_lease->map_into(retired, 0, host::Access::read_write);
    check(retired.data()[3] == std::byte{0} && current.data()[3] == std::byte{0xbb},
          "a reclaimed backing offset is zero-filled and cannot clear an adjacent live page");
    retired.decommit(0, physical_page_bytes);
    reused_lease.reset();
    (void)pages.free(reused_storage.pages);
    current.decommit(0, physical_page_bytes);
    live.reset();
    const std::array remaining{first.pages[1], replacement.pages[0]};
    check(!pages.free(remaining) && pages.available() == capacity,
          "freeing all IDs and aliases restores the exact bounded backing capacity");
    check(pages.allocate(0x280, capacity + 1).error == error_not_enough_memory &&
              pages.allocate(0x280, UINT64_MAX).error == error_not_enough_memory &&
              pages.available() == capacity,
          "oversized counts fail without partial publication or unsigned-size overflow");
    auto measured = pages.allocate(0x280, 11360);
    check(!measured.error && measured.pages.size() == 11360 && pages.available() == capacity - 11360 &&
              std::all_of(
                  measured.pages.begin(), measured.pages.end(),
                  [](auto token) { return token && token % 16 == 0 && token < (std::uint64_t{1} << 25); }),
          "the measured710MiB request receives exactly its budgeted leases and valid packed IDs");
    check(!pages.free(measured.pages) && pages.available() == capacity,
          "releasing a measured-size allocation restores capacity without filling a large memory buffer");
    auto partial = pages.allocate(0x280, 2);
    if (partial.error || partial.pages.size() != 2) {
        check(false, "partial-free fixture allocation succeeds");
        return;
    }
    const std::array duplicate{partial.pages[0], partial.pages[0]};
    const auto partial_error = pages.free(duplicate);
    const bool first_retired = !pages.lease(partial.pages[0]);
    const bool second_retained = static_cast<bool>(pages.lease(partial.pages[1]));
    const auto remaining_error = pages.free(std::span(partial.pages).subspan(1));
    check(partial_error == error_invalid_parameter && first_retired && second_retained && !remaining_error &&
              pages.available() == capacity,
          "an invalid free stops after its valid prefix and never frees a different live token");
    auto final = pages.allocate(0x280, 1);
    auto retained = final.pages.empty() ? nullptr : pages.lease(final.pages[0]);
    if (!retained) {
        check(false, "the allocator lifetime fixture obtains a page lease");
        return;
    }
    retained->map_into(retired, 0, host::Access::read_write);
    retired.data()[3] = std::byte{0x79};
    owner.reset();
    retained->map_into(current, 0, host::Access::read_write);
    check(retired.data()[3] == std::byte{0x79} && current.data()[3] == std::byte{0x79},
          "an outstanding lease retains backing ownership and new aliases after its allocator is destroyed");
    retired.decommit(0, physical_page_bytes);
    current.decommit(0, physical_page_bytes);
    retained.reset();
}

void physical_page_worker_check(int descriptor) {
    using namespace ambrosia;
    using namespace services;
    PhysicalPages pages;
    pages.bind(host::SharedBacking::adopt_descriptor(descriptor, PhysicalPages::maximum_backing_bytes));
    std::mutex active_mutex;
    std::set<std::uint64_t> active;
    std::atomic<bool> valid{true};
    const auto work = [&] {
        try {
            for (unsigned iteration = 0; iteration < 32; ++iteration) {
                auto allocated = pages.allocate(0x280, 2);
                if (allocated.error || allocated.pages.size() != 2) {
                    valid = false;
                    return;
                }
                {
                    const std::lock_guard lock(active_mutex);
                    for (const auto token : allocated.pages) {
                        if (!active.insert(token).second) {
                            valid = false;
                        }
                    }
                }
                std::this_thread::yield();
                {
                    const std::lock_guard lock(active_mutex);
                    for (const auto token : allocated.pages) {
                        if (active.erase(token) != 1) {
                            valid = false;
                        }
                    }
                    if (pages.free(allocated.pages)) {
                        valid = false;
                    }
                }
            }
        } catch (...) {
            valid = false;
        }
    };
    std::jthread first(work);
    std::jthread second(work);
    first.join();
    second.join();
    check(valid.load() && active.empty() && pages.available() == pages.capacity(),
          "joined native allocator workers never share a live ID and return every backing lease");
}
