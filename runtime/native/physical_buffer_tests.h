void physical_buffer_checks(int descriptor, const std::filesystem::path &root) {
    using namespace ambrosia;
    using namespace services;
    constexpr auto page = physical_page_bytes;
    PhysicalPages physical;
    physical.bind(host::SharedBacking::adopt_descriptor(descriptor, PhysicalPages::maximum_backing_bytes));
    const auto capacity = physical.available();
    MemoryServices memory(page * 8);
    const auto allocation = physical.allocate(0x280, 2);
    check(!allocation.error && allocation.pages.size() == 2, "physical buffer fixture owns actual backing");
    if (allocation.error || allocation.pages.size() != 2) {
        return;
    }
    std::array<std::shared_ptr<const PhysicalPage>, 2> leases{physical.lease(allocation.pages[0]),
                                                              physical.lease(allocation.pages[1])};
    auto *first = static_cast<std::byte *>(memory.reserve_window(page, 0x27, 0x1004).address);
    auto *second = static_cast<std::byte *>(memory.reserve_window(page, 0x27, 0x2404).address);
    auto *alias = static_cast<std::byte *>(memory.reserve_window(page, 3, 2).address);
    check(first && second && alias && memory.map_physical(first, std::span(leases).first(1)) == 0 &&
              memory.map_physical(second, std::span(leases).subspan(1)) == 0 &&
              memory.map_physical(alias, std::span(leases).subspan(1)) == 0,
          "GPU copy destinations include two CPU aliases of the same physical page");
    const auto source_span = memory.physical_span(first, page);
    const auto destination_span = memory.physical_span(second, page);
    const auto alias_span = memory.physical_span(alias, page);
    if (!source_span.span || !destination_span.span || !alias_span.span) {
        throw std::runtime_error("physical buffer fixture has incomplete mapped spans");
    }
    PhysicalSpanView readonly(*source_span.span);
    check(runtime_rejected([&] { (void)readonly.writable_data(); }),
          "default physical views cannot return writable pointers");
    bool executable_rejected = false;
    try {
        PhysicalSpanView bad(*source_span.span, host::Access::read_execute);
    } catch (const std::invalid_argument &) {
        executable_rejected = true;
    }
    check(executable_rejected, "retained views cannot acquire executable access");
    host::Graphics graphics(root);
    const auto device = graphics.create_device();
    {
        PhysicalBuffer source(graphics, *source_span.span);
        PhysicalBuffer destination(graphics, *destination_span.span);
        PhysicalBuffer destination_alias(graphics, *alias_span.span);
        check(source.cpu_writable() && !destination_alias.cpu_writable() &&
                  runtime_rejected([&] { (void)destination_alias.cpu_data(); }),
              "a GPU writeback alias cannot expose a writable CPU map over read-only policy");
        std::fill(first, first + 4096, std::byte{0x45});
        source.copy_to(destination, 0, 0, 4096);
        check(second[0] == std::byte{0x45} && alias[4095] == std::byte{0x45},
              "CPU writes after creation reach the real GPU and write back through every physical alias");
        std::fill(first + 8192, first + 12288, std::byte{});
        destination_alias.copy_to(source, 0, 8192, 4096);
        check(first[8192] == std::byte{0x45} && first[12287] == std::byte{0x45},
              "a separate resource alias refreshes from canonical physical bytes before its next GPU read");
        check(runtime_rejected([&] { source.copy_to(source, 0, 4, 16); }) &&
                  runtime_rejected([&] { source.copy_to(destination, 1, 0, 4); }) &&
                  runtime_rejected([&] { source.upload(SIZE_MAX, 1); }),
              "invalid physical copy and transfer ranges fail before any GPU operation");
        const std::array<std::shared_ptr<const PhysicalPage>, 1> empty{};
        check(memory.map_physical(first, empty) == 0 && memory.map_physical(second, empty) == 0 &&
                  memory.map_physical(alias, empty) == 0 && physical.free(allocation.pages) == 0,
              "original mappings and token ownership can retire while buffers retain the exact pages");
        leases.fill(nullptr);
        source.cpu_data()[0] = std::byte{0x67};
        source.copy_to(destination, 0, 0, 4);
        check(destination.cpu_data()[0] == std::byte{0x67} && physical.available() == capacity - 2,
              "GPU transfer remains coherent after original unmap and token retirement");
    }
    graphics.destroy_device(device.id);
    // The explicit snapshots and read-only view still retain backing here.
    check(physical.available() == capacity - 2,
          "caller snapshots retain backing independently of GPU owners");
}
