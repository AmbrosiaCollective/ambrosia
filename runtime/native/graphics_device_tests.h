void graphics_option_checks() {
    const auto rejects = [&](std::vector<std::filesystem::path> arguments, const char *message) {
        std::ostringstream output, errors;
        return ambrosia::run_native_command(arguments, output, errors) == 2 &&
               errors.str().find(message) != std::string::npos;
    };
    check(rejects({"--trace-startup", "unused", "--graphics-root"}, "requires a value"),
          "graphics-root requires an explicit value before touching files");
    check(rejects({"--trace-startup", "unused", "--graphics-root", "one", "--graphics-root", "two"},
                  "duplicate graphics-root"),
          "graphics-root rejects duplicate service selection");
    check(rejects({"--inspect", "unused", "--graphics-root", "service"}, "graphics-root requires a run"),
          "graphics-root cannot change inspection into host integration");
}

void graphics_device_live_checks(const std::filesystem::path &root, unsigned probe = 0) {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    Scope scope(runtime);
    guest::ThreadContext context;
    auto activation = context.activate();
    runtime.graphics_devices().bind(root);
    using Create = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, const void *, const void *, void **);
    const auto create =
        reinterpret_cast<Create>(runtime.resolve(service_symbol("D3D12XboxCreateDevice", "d3d12_xs.dll")));
    const Guid unknown{};
    void *device = reinterpret_cast<void *>(1);
    check(create(nullptr, nullptr, &unknown, &device) == no_interface && !device,
          "unknown device interfaces fail without publishing a fabricated device");
    check(create(reinterpret_cast<void *>(1), nullptr, &device11_interface, &device) ==
                  static_cast<std::int32_t>(0x80070057u) &&
              !device,
          "unsupported adapter selection fails before native device creation");
    std::array<std::byte, 32> plan{};
    plan[4] = std::byte{3};
    check(runtime.graphics_memory().configure(plan) == 0,
          "rejected creation does not freeze the graphics memory policy");
    const auto saved_errno = errno;
    errno = E2BIG;
    set_last_error(73);
    crt_error() = 29;
    check(create(nullptr, nullptr, &device11_interface, &device) == 0 && device && errno == E2BIG &&
              get_last_error() == 73 && crt_error() == 29,
          "device creation publishes the real backend frontend and preserves guest error state");
    auto *table = *static_cast<const std::uintptr_t **>(device);
    using Query = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, const void *, void **);
    using Reference = std::uint32_t(AMBROSIA_GUEST_ABI *)(void *);
    const auto query = reinterpret_cast<Query>(table[0]);
    const auto add = reinterpret_cast<Reference>(table[1]);
    const auto release = reinterpret_cast<Reference>(table[2]);
    const auto nodes = reinterpret_cast<Reference>(table[7]);
    if (probe == 1) {
        reinterpret_cast<Reference>(table[8])(device);
        throw std::runtime_error("unsupported device slot unexpectedly returned");
    }
    if (probe == 2) {
        Runtime other;
        Scope foreign_scope(other);
        (void)add(device);
        throw std::runtime_error("foreign-runtime device unexpectedly accepted");
    }
    void *queried = reinterpret_cast<void *>(1);
    check(query(device, nullptr, &queried) == pointer_error && !queried &&
              query(device, &unknown, nullptr) == pointer_error,
          "device QueryInterface validates output and IID pointers before access");
    check(query(device, &unknown, &queried) == no_interface && !queried,
          "device QueryInterface rejects unsupported identities without an added reference");
    check(query(device, &device11_interface, &queried) == 0 && queried == device && release(queried) == 1,
          "device identity is stable and QueryInterface owns its returned reference");
    check(query(device, &unknown_interface_id, &queried) == 0 && queried == device && release(queried) == 1,
          "device IUnknown identity shares the real object's reference count");
    check(add(device) == 2 && release(device) == 1,
          "device AddRef and Release preserve independent guest references");
    check(nodes(device) == 1 && errno == E2BIG && get_last_error() == 73 && crt_error() == 29,
          "GetNodeCount validates the real one-device backend and preserves guest errors");
    check(release(device) == 0, "the final guest reference releases actual native device ownership");
    device = nullptr;
    check(create(nullptr, nullptr, &device11_interface, &device) == 0 && device && nodes(device) == 1,
          "device creation can acquire a new real backend owner after release");
    check(release(device) == 0, "recreated devices also release their native ownership");
    errno = saved_errno;
}
