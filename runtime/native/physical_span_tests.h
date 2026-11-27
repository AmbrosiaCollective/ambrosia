void physical_span_checks(int descriptor) {
    using namespace ambrosia;
    using namespace services;
    constexpr std::size_t page = physical_page_bytes;
    PhysicalPages physical;
    physical.bind(host::SharedBacking::adopt_descriptor(descriptor, PhysicalPages::maximum_backing_bytes));
    const auto capacity = physical.available();
    std::optional<MemoryServices> memory;
    memory.emplace(page * 8);
    const auto allocation = physical.allocate(0x280, 3);
    check(!allocation.error && allocation.pages.size() == 3,
          "span fixture owns three distinct physical pages");
    if (allocation.error || allocation.pages.size() != 3) {
        return;
    }
    std::vector<std::shared_ptr<const PhysicalPage>> leases;
    for (const auto token : allocation.pages) {
        leases.push_back(physical.lease(token));
    }
    const auto parent = memory->reserve_placeholder(page * 3, 7);
    auto *original = static_cast<std::byte *>(parent.address);
    check(original && memory->free(original, page, 0x8002).success &&
              memory->replace_placeholder_window(original, page, 3, 0x2004).address == original &&
              memory->replace_placeholder_window(original + page, page * 2, 0x27, 0x1004).address ==
                  original + page &&
              memory->map_physical(original, std::span(leases).first(1)) == 0 &&
              memory->map_physical(original + page, std::span(leases).subspan(1)) == 0,
          "adjacent windows retain distinct policy while mapping one logical byte range");
    if (!original) {
        return;
    }
    auto captured = memory->physical_span(original + 101, page * 2);
    check(captured.span && !captured.error && captured.span->size() == page * 2 &&
              captured.span->first_page_offset() == 101 && captured.span->pages().size() == 3 &&
              captured.span->pages()[0].attributes == 3 && captured.span->pages()[0].protection == 0x2004 &&
              captured.span->pages()[1].attributes == 0x27 && captured.span->pages()[1].protection == 0x1004,
          "a retained span crosses child windows and preserves every page's original policy");
    if (!captured.span) {
        return;
    }
    original[101] = std::byte{0x34};
    original[page + 13] = std::byte{0x56};
    original[page * 2 + 100] = std::byte{0x78};
    auto view = std::make_unique<PhysicalSpanView>(*captured.span);
    check(view->size() == page * 2 && view->data()[0] == std::byte{0x34} &&
              view->data()[page - 88] == std::byte{0x56} && view->data()[page * 2 - 1] == std::byte{0x78} &&
              inspection_page_permissions(view->data()).starts_with("r--"),
          "the retained view reads coherent cross-window bytes with its exact prefix and read-only access");
    original[101] = std::byte{0x45};
    check(view->data()[0] == std::byte{0x45}, "retained views observe writes to the same physical pages");
    check(memory->physical_span(nullptr, 1).error == error_invalid_parameter &&
              memory->physical_span(original, 0).error == error_invalid_parameter &&
              memory->physical_span(original, SIZE_MAX).error == error_invalid_parameter &&
              memory->physical_span(reinterpret_cast<void *>(UINTPTR_MAX - 10), 20).error ==
                  error_invalid_parameter &&
              memory->physical_span(original + page * 2, page * 2).error == error_invalid_address,
          "null, empty, overflowing and out-of-owner spans fail without a large allocation");
    const auto ordinary = memory->allocate(nullptr, page, 0x3000, 4);
    check(ordinary.address && !memory->physical_span(ordinary.address, page).span,
          "a private allocation cannot masquerade as retained physical backing");
    const std::array<std::shared_ptr<const PhysicalPage>, 1> unmapped{};
    check(memory->map_physical(original, unmapped) == 0 && !memory->physical_span(original, page * 2).span &&
              view->data()[0] == std::byte{0x45},
          "an unmapped gap rejects a fresh whole-span capture while prior retained views survive");
    check(physical.free(allocation.pages) == 0 && physical.available() == capacity - 3,
          "retained snapshots keep physical backing after token retirement");
    leases.clear();
    auto replacement = physical.allocate(0x280, 1);
    check(!replacement.error && replacement.pages.size() == 1,
          "new physical storage remains available while old snapshots are retained");
    if (replacement.error || replacement.pages.size() != 1) {
        return;
    }
    std::array<std::shared_ptr<const PhysicalPage>, 1> replacement_lease{
        physical.lease(replacement.pages[0])};
    const auto remapped = memory->map_physical(original, replacement_lease);
    check(remapped == 0 && original[101] == std::byte{} && view->data()[0] == std::byte{0x45},
          "a remapped original address cannot alter the snapshot's stable physical identity");
    memory.reset();
    replacement_lease[0].reset();
    check(physical.free(replacement.pages) == 0 && physical.available() == capacity - 3 &&
              view->data()[page * 2 - 1] == std::byte{0x78},
          "the view remains valid after original reservations and new allocation identities are destroyed");
    view.reset();
    check(physical.available() == capacity - 3,
          "the snapshot itself retains pages even after its optional CPU view closes");
    captured.span.reset();
    check(physical.available() == capacity,
          "destroying the final snapshot returns every retired physical page to the backing pool");
    bool invalid_view_rejected = false;
    try {
        PhysicalSpanView invalid(PhysicalSpan{});
    } catch (const std::invalid_argument &) {
        invalid_view_rejected = true;
    }
    check(invalid_view_rejected, "an empty span cannot create a host view");
}
