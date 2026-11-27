void descriptor_increment_live_checks(const std::filesystem::path &root, unsigned probe) {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    Scope scope(runtime);
    guest::ThreadContext context;
    auto activation = context.activate();
    runtime.graphics_devices().bind(root);
    void *device = nullptr;
    check(runtime.graphics_devices().create(&device11_interface, &device, std::nullopt) == 0 && device,
          "descriptor metadata belongs to a real backend device");
    const auto *table = *static_cast<const std::uintptr_t **>(device);
    using Increment = std::uint32_t(AMBROSIA_GUEST_ABI *)(void *, std::uint32_t);
    using Release = std::uint32_t(AMBROSIA_GUEST_ABI *)(void *);
    const auto increment = reinterpret_cast<Increment>(table[15]);
    const auto release = reinterpret_cast<Release>(table[2]);
    if (probe == 1) {
        Runtime other;
        Scope foreign(other);
        (void)increment(device, 0);
        throw std::runtime_error("foreign descriptor query unexpectedly returned");
    }
    if (probe == 2) {
        (void)release(device);
        (void)increment(device, 0);
        throw std::runtime_error("released descriptor query unexpectedly returned");
    }
    if (probe == 3) {
        (void)increment(device, 5);
        throw std::runtime_error("unknown descriptor kind unexpectedly returned");
    }
    const auto saved_errno = errno;
    errno = E2BIG;
    set_last_error(89);
    crt_error() = 37;
    check(increment(device, 0) == 32 && increment(device, 1) == 16 && increment(device, 2) == 32 &&
              increment(device, 3) == 32,
          "public heap kinds query independent guest record strides through the correct ABI");
    check(increment(device, 4) == 32,
          "the console extra view kind queries the published normal view-record stride");
    check(errno == E2BIG && get_last_error() == 89 && crt_error() == 37,
          "descriptor metadata preserves native and guest errors");
    check(release(device) == 0, "descriptor metadata introduces no additional device lease");
    errno = saved_errno;
}
