void placed_resource_capture_check(const std::filesystem::path &root) {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    Scope scope(runtime);
    guest::ThreadContext context;
    auto activation = context.activate();
    runtime.graphics_devices().bind(root);
    void *device = nullptr;
    check(runtime.graphics_devices().create(&device11_interface, &device, std::nullopt) == 0 && device,
          "placed-resource diagnostic owns a real backend device");
    // Independent synthetic values exercise all public description field offsets.
    std::array<std::byte, 56> description{};
    const auto store = [&](std::size_t offset, auto value) {
        std::memcpy(description.data() + offset, &value, sizeof(value));
    };
    store(0, std::uint32_t{1});
    store(8, std::uint64_t{65536});
    store(16, std::uint64_t{65536});
    store(24, std::uint32_t{1});
    store(28, std::uint16_t{1});
    store(30, std::uint16_t{1});
    store(32, std::uint32_t{0});
    store(36, std::uint32_t{1});
    store(40, std::uint32_t{0});
    store(44, std::uint32_t{1});
    store(48, std::uint32_t{0x20004});
    const auto window = runtime.memory().reserve_window(65536, 0x27, 0x2404);
    check(window.address && !window.error, "placed-resource diagnostic owns its requested synthetic window");
    if (!window.address) {
        return;
    }
    const auto *table = *static_cast<const std::uintptr_t **>(device);
    using Capture = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, std::uint64_t, const void *, std::uint32_t,
                                                       const void *, const void *, void **);
    void *output = nullptr;
    diagnostics::ImportStops stops;
    stops.seal();
    diagnostics::ImportStopScope trace(stops);
    (void)reinterpret_cast<Capture>(table[133])(device, reinterpret_cast<std::uintptr_t>(window.address),
                                                description.data(), UINT32_MAX, nullptr, &resource_interface,
                                                &output);
    throw std::runtime_error("placed-resource capture unexpectedly returned a resource");
}
