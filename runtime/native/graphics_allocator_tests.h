void graphics_allocator_live_checks(const std::filesystem::path &root, unsigned probe = 0) {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    Scope scope(runtime);
    guest::ThreadContext context;
    auto activation = context.activate();
    runtime.graphics_devices().bind(root);
    void *device = nullptr;
    check(runtime.graphics_devices().create(&device11_interface, &device, std::nullopt) == 0 && device,
          "allocator fixture owns a real backend device");
    auto *device_table = *static_cast<const std::uintptr_t **>(device);
    using CreateAllocator = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, std::uint32_t, const void *, void **);
    using Query = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, const void *, void **);
    using Reference = std::uint32_t(AMBROSIA_GUEST_ABI *)(void *);
    using Reset = std::int32_t(AMBROSIA_GUEST_ABI *)(void *);
    const auto create_allocator = reinterpret_cast<CreateAllocator>(device_table[9]);
    const auto release_device = reinterpret_cast<Reference>(device_table[2]);
    const auto saved_errno = errno;
    errno = E2BIG;
    set_last_error(83);
    crt_error() = 31;
    check(create_allocator(device, 0, reinterpret_cast<void *>(1), nullptr) == pointer_error,
          "allocator creation validates output before reading interface memory");
    void *output = reinterpret_cast<void *>(1);
    check(create_allocator(device, 0, nullptr, &output) == pointer_error && !output,
          "allocator creation clears output before rejecting a null interface");
    const Guid unknown{};
    check(create_allocator(device, 0, &unknown, &output) == no_interface && !output,
          "an unsupported allocator interface creates no owned object");
    check(create_allocator(device, UINT32_MAX, &command_allocator_interface, &output) ==
                  static_cast<std::int32_t>(0x80070057u) &&
              !output,
          "an invalid command-list type is rejected without changing ownership");
    check(create_allocator(device, 4, &command_allocator_interface, &output) ==
                  static_cast<std::int32_t>(0x80004001u) &&
              !output,
          "valid video allocation remains explicitly unsupported");
    std::array<void *, 4> allocators{};
    for (std::uint32_t type = 0; type < allocators.size(); ++type) {
        check(create_allocator(device, type, &command_allocator_interface, &allocators[type]) == 0 &&
                  allocators[type],
              "each supported CPU allocator kind retains its real parent device");
    }
    if (probe == 4) {
        using CreateListX = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, const void *, void *, const void *,
                                                               const void *, void **);
        const auto create_list_x = reinterpret_cast<CreateListX>(device_table[73]);
        const std::array<std::uint32_t, 4> description{};
        diagnostics::ImportStops synthetic_stops;
        synthetic_stops.seal();
        diagnostics::ImportStopScope trace_scope(synthetic_stops);
        (void)create_list_x(device, description.data(), allocators[0], nullptr, &unknown_interface_id,
                            &output);
        throw std::runtime_error("diagnostic command-list creation unexpectedly returned");
    }
    auto *table = *static_cast<const std::uintptr_t **>(allocators[0]);
    const auto query = reinterpret_cast<Query>(table[0]);
    const auto add = reinterpret_cast<Reference>(table[1]);
    const auto release = reinterpret_cast<Reference>(table[2]);
    const auto get_device = reinterpret_cast<Query>(table[7]);
    const auto reset = reinterpret_cast<Reset>(table[8]);
    if (probe == 1) {
        reinterpret_cast<Reset>(table[3])(allocators[0]);
        throw std::runtime_error("unsupported allocator slot unexpectedly returned");
    }
    if (probe == 2) {
        Runtime other;
        Scope foreign(other);
        (void)reset(allocators[0]);
        throw std::runtime_error("foreign allocator unexpectedly reset");
    }
    check(query(allocators[0], nullptr, &output) == pointer_error && !output &&
              query(allocators[0], &command_allocator_interface, nullptr) == pointer_error,
          "allocator QueryInterface validates both pointer arguments");
    check(query(allocators[0], &unknown, &output) == no_interface && !output,
          "allocator QueryInterface refuses unknown identities without a reference");
    check(query(allocators[0], &command_allocator_interface, &output) == 0 && output == allocators[0] &&
              release(output) == 1 && add(allocators[0]) == 2 && release(allocators[0]) == 1,
          "allocator identity and references share the owned CPU object");
    check(query(allocators[0], &unknown_interface_id, &output) == 0 && output == allocators[0] &&
              release(output) == 1,
          "allocator IUnknown returns the same identity with an owned reference");
    check(reset(allocators[0]) == 0 && reset(allocators[0]) == 0,
          "an empty CPU allocator can be reset repeatedly without claiming GPU work");
    check(get_device(allocators[0], nullptr, &output) == pointer_error && !output &&
              get_device(allocators[0], &unknown, &output) == no_interface && !output,
          "allocator GetDevice validates and refuses unsupported parent identities");
    check(release_device(device) == allocators.size(),
          "allocator children independently retain their parent");
    check(get_device(allocators[0], &device11_interface, &output) == 0 && output == device &&
              release_device(output) == allocators.size(),
          "allocator GetDevice returns its live parent with a separate reference");
    check(errno == E2BIG && get_last_error() == 83 && crt_error() == 31,
          "allocator ABI calls preserve native and guest error states");
    for (auto *allocator : allocators) {
        auto *entries = *static_cast<const std::uintptr_t **>(allocator);
        check(reinterpret_cast<Reference>(entries[2])(allocator) == 0,
              "each last allocator reference releases exactly one parent lease");
    }
    if (probe == 3) {
        (void)reset(allocators[0]);
        throw std::runtime_error("released allocator unexpectedly reset");
    }
    device = nullptr;
    check(runtime.graphics_devices().create(&device11_interface, &device, std::nullopt) == 0 && device &&
              release_device(device) == 0,
          "last child teardown releases real backend ownership for another device");
    errno = saved_errno;
}
