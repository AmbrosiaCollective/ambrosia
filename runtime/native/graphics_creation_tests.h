void graphics_creation_checks() {
    using namespace ambrosia::services;
    Runtime runtime;
    Scope scope(runtime);
    using Create = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, const void *, const void *, void **);
    const auto address = runtime.resolve(service_symbol("D3D12XboxCreateDevice", "d3d12_xs.dll"));
    check(address != 0, "the native graphics entry exposes the four-argument Xbox device ABI");
    if (!address) {
        return;
    }
    const auto create = reinterpret_cast<Create>(address);
    const auto saved_errno = errno;
    set_last_error(73);
    crt_error() = 29;
    errno = E2BIG;
    constexpr auto pointer_error = static_cast<std::int32_t>(0x80004003u);
    check(create(reinterpret_cast<void *>(1), reinterpret_cast<const void *>(1),
                 reinterpret_cast<const void *>(1), nullptr) == pointer_error &&
              errno == E2BIG && get_last_error() == 73 && crt_error() == 29,
          "a null graphics output fails before reading any other pointer and preserves thread errors");

    struct GuardedOutput {
        std::uintptr_t before = 0x12345678;
        void *value = reinterpret_cast<void *>(1);
        std::uintptr_t after = 0x87654321;
    } output;

    check(create(nullptr, reinterpret_cast<const void *>(1), nullptr, &output.value) == pointer_error &&
              !output.value && output.before == 0x12345678 && output.after == 0x87654321 && errno == E2BIG &&
              get_last_error() == 73 && crt_error() == 29,
          "a null graphics IID clears only the output slot without reading the parameter block");
    errno = saved_errno;
}

int graphics_creation_probe() {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    Scope scope(runtime);
    guest::ThreadContext context;
    auto activation = context.activate();
    using Create = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, const void *, const void *, void **);
    const auto create =
        reinterpret_cast<Create>(runtime.resolve(service_symbol("D3D12XboxCreateDevice", "d3d12_xs.dll")));
    const std::array<std::byte, 16> identifier{};
    const std::array<std::uint32_t, 24> parameters{};
    void *output = nullptr;
    (void)create(nullptr, parameters.data(), identifier.data(), &output);
    return 99;
}
