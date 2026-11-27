void placed_buffer_checks(int descriptor, const std::filesystem::path &root, unsigned probe) {
    using namespace ambrosia;
    using namespace services;
    constexpr auto page = physical_page_bytes;
    Runtime runtime(page * 8);
    Scope scope(runtime);
    guest::ThreadContext context;
    auto activation = context.activate();
    runtime.physical_pages().bind(
        host::SharedBacking::adopt_descriptor(descriptor, PhysicalPages::maximum_backing_bytes));
    runtime.graphics_devices().bind(root);
    void *device = nullptr;
    check(runtime.graphics_devices().create(&device11_interface, &device, std::nullopt) == 0 && device,
          "placed buffer fixture owns a real graphics device");
    auto *device_table = *static_cast<const std::uintptr_t **>(device);
    using Create = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, std::uint64_t, const void *, std::uint32_t,
                                                      const void *, const void *, void **);
    using Query = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, const void *, void **);
    using Reference = std::uint32_t(AMBROSIA_GUEST_ABI *)(void *);
    using Map = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, std::uint32_t, const void *, void **);
    using Unmap = void(AMBROSIA_GUEST_ABI *)(void *, std::uint32_t, const void *);
    using Description = ResourceDescription *(AMBROSIA_GUEST_ABI *)(void *, ResourceDescription *);
    using AggregateDescription = ResourceDescription(AMBROSIA_GUEST_ABI *)(void *);
    using Address = std::uint64_t(AMBROSIA_GUEST_ABI *)(void *);
    const auto create = reinterpret_cast<Create>(device_table[133]);
    const auto release_device = reinterpret_cast<Reference>(device_table[2]);
    const auto allocation = runtime.physical_pages().allocate(0x280, 2);
    if (allocation.error || allocation.pages.size() != 2) {
        throw std::runtime_error("placed fixture physical allocation failed");
    }
    std::array<std::shared_ptr<const PhysicalPage>, 2> leases{
        runtime.physical_pages().lease(allocation.pages[0]),
        runtime.physical_pages().lease(allocation.pages[1])};
    auto *original =
        static_cast<std::byte *>(runtime.memory().reserve_window(page * 2, 0x27, 0x1004).address);
    check(original && runtime.memory().map_physical(original, leases) == 0,
          "the full placed buffer range has real mapped physical backing");
    if (!original) {
        throw std::runtime_error("placed fixture has no original window");
    }
    original[0] = std::byte{0x28};
    ResourceDescription description{1, 65536, page * 2, 1, 1, 1, 0, 1, 0, 1, 0x20004};
    void *resource = reinterpret_cast<void *>(1);
    const auto address = reinterpret_cast<std::uintptr_t>(original);
    check(create(device, address, nullptr, UINT32_MAX, nullptr, &resource_interface, nullptr) ==
                  pointer_error &&
              create(device, address, nullptr, UINT32_MAX, nullptr, &resource_interface, &resource) ==
                  pointer_error &&
              !resource,
          "placed creation validates output and description pointers before reading caller data");
    const Guid unknown{};
    check(create(device, address, &description, UINT32_MAX, nullptr, &unknown, &resource) == no_interface &&
              !resource,
          "an unknown creation identity cannot allocate or return a resource");
    const auto saved_errno = errno;
    errno = E2BIG;
    set_last_error(89);
    crt_error() = 37;
    check(create(device, address, &description, UINT32_MAX, nullptr, &resource_interface, &resource) == 0 &&
              resource && errno == E2BIG && get_last_error() == 89 && crt_error() == 37,
          "placed creation initializes a real GPU buffer while preserving guest error state");
    auto *table = *static_cast<const std::uintptr_t **>(resource);
    const auto query = reinterpret_cast<Query>(table[0]);
    const auto add = reinterpret_cast<Reference>(table[1]);
    const auto release = reinterpret_cast<Reference>(table[2]);
    const auto get_device = reinterpret_cast<Query>(table[7]);
    const auto map = reinterpret_cast<Map>(table[8]);
    const auto unmap = reinterpret_cast<Unmap>(table[9]);
    if (probe == 1) {
        reinterpret_cast<Reference>(table[6])(resource);
        throw std::runtime_error("unsupported resource slot returned");
    }
    if (probe == 2) {
        Runtime other;
        Scope foreign(other);
        (void)add(resource);
        throw std::runtime_error("foreign runtime retained a resource");
    }
    void *output = reinterpret_cast<void *>(1);
    check(query(resource, &unknown, &output) == no_interface && !output &&
              query(resource, nullptr, &output) == pointer_error && !output,
          "resource identity checks clear output before rejection");
    check(query(resource, &resource_interface, &output) == 0 && output == resource && release(output) == 1 &&
              add(resource) == 2 && release(resource) == 1,
          "resource identity and references retain the same owned object");
    check(get_device(resource, &device11_interface, &output) == 0 && output == device &&
              release_device(output) == 2,
          "a resource retains and returns its actual parent device");
    ResourceDescription c_description;
    check(reinterpret_cast<Description>(table[10])(resource, &c_description) == &c_description &&
              c_description.width == description.width && c_description.flags == description.flags &&
              c_description.alignment == description.alignment,
          "C-style GetDesc preserves the original console description and raw flags");
    const auto aggregate = reinterpret_cast<AggregateDescription>(table[10])(resource);
    check(aggregate.width == description.width && aggregate.flags == description.flags &&
              reinterpret_cast<Address>(table[11])(resource) == address,
          "aggregate-return GetDesc and console GPU virtual address use their actual ABIs");
    check(map(resource, 0, nullptr, &output) == 0 && output == original,
          "Map preserves original console address identity while the same pages remain mapped");
    static_cast<std::byte *>(output)[1] = std::byte{0x39};
    const ResourceRange written{1, 2};
    unmap(resource, 0, &written);
    check(original[1] == std::byte{0x39}, "Unmap uploads the written range without detaching CPU aliases");
    const ResourceRange invalid{2, 1};
    check(map(resource, 1, nullptr, &output) == static_cast<std::int32_t>(0x80070057u) && !output &&
              map(resource, 0, &invalid, &output) == static_cast<std::int32_t>(0x80070057u) && !output,
          "unsupported subresources and reversed ranges cannot return a CPU pointer");
    const std::array<std::shared_ptr<const PhysicalPage>, 2> empty{};
    check(runtime.memory().map_physical(original, empty) == 0 &&
              runtime.physical_pages().free(allocation.pages) == 0,
          "placed resources retain physical pages after original unmap and token retirement");
    leases.fill(nullptr);
    check(map(resource, 0, nullptr, &output) == 0 && output != original &&
              static_cast<std::byte *>(output)[0] == std::byte{0x28} &&
              static_cast<std::byte *>(output)[1] == std::byte{0x39},
          "Map falls back to the retained physical alias after the original mapping disappears");
    check(release_device(device) == 1 && release(resource) == 0 &&
              runtime.physical_pages().available() == runtime.physical_pages().capacity(),
          "final resource release retires GPU allocation, aliases, physical leases and its parent device");
    errno = saved_errno;
}
