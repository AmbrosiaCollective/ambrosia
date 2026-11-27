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

void graphics_fence_live_checks(const std::filesystem::path &root, unsigned probe) {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    Scope scope(runtime);
    guest::ThreadContext context;
    auto activation = context.activate();
    runtime.graphics_devices().bind(root);
    using CreateDevice = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, const void *, const void *, void **);
    const auto create_device = reinterpret_cast<CreateDevice>(
        runtime.resolve(service_symbol("D3D12XboxCreateDevice", "d3d12_xs.dll")));
    void *device = nullptr;
    check(create_device(nullptr, nullptr, &device11_interface, &device) == 0 && device,
          "fence fixture owns a real native device");
    auto *device_table = *static_cast<const std::uintptr_t **>(device);
    using CreateFence =
        std::int32_t(AMBROSIA_GUEST_ABI *)(void *, std::uint64_t, std::uint32_t, const void *, void **);
    using Query = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, const void *, void **);
    using Reference = std::uint32_t(AMBROSIA_GUEST_ABI *)(void *);
    using Counter = std::uint64_t(AMBROSIA_GUEST_ABI *)(void *);
    using Signal = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, std::uint64_t);
    const auto create_fence = reinterpret_cast<CreateFence>(device_table[36]);
    const auto release_device = reinterpret_cast<Reference>(device_table[2]);
    const auto saved_errno = errno;
    errno = E2BIG;
    set_last_error(71);
    crt_error() = 19;
    check(create_fence(device, 7, 0, reinterpret_cast<const void *>(1), nullptr) == pointer_error,
          "CreateFence validates output before reading interface identity");
    void *fence = reinterpret_cast<void *>(1);
    check(create_fence(device, 7, 0, nullptr, &fence) == pointer_error && !fence,
          "CreateFence clears only the output before rejecting a null IID");
    const Guid unsupported{};
    check(create_fence(device, 7, 0, &unsupported, &fence) == no_interface && !fence,
          "CreateFence rejects unsupported identity without making a host semaphore");
    check(create_fence(device, 7, probe == 3 ? 1 : 0, &fence_interface, &fence) == 0 && fence,
          "CreateFence publishes a real timeline-backed guest fence");
    auto *table = *static_cast<const std::uintptr_t **>(fence);
    const auto query = reinterpret_cast<Query>(table[0]);
    const auto add = reinterpret_cast<Reference>(table[1]);
    const auto release = reinterpret_cast<Reference>(table[2]);
    const auto get_device = reinterpret_cast<Query>(table[7]);
    const auto completed = reinterpret_cast<Counter>(table[8]);
    const auto signal = reinterpret_cast<Signal>(table[10]);
    if (probe == 1) {
        reinterpret_cast<Reference>(table[9])(fence);
        throw std::runtime_error("unsupported fence event method unexpectedly returned");
    }
    if (probe == 2) {
        (void)signal(fence, 6);
        throw std::runtime_error("decreasing timeline fence signal unexpectedly returned");
    }
    check(completed(fence) == 7, "fence completion reads the actual initial Vulkan counter");
    check(signal(fence, std::uint64_t{1} << 40) == 0 && completed(fence) == (std::uint64_t{1} << 40),
          "fence host signal and completion preserve values above 32 bits");
    check(signal(fence, std::uint64_t{1} << 40) == 0 &&
              signal(fence, UINT64_MAX) == static_cast<std::int32_t>(0x80070057u) &&
              completed(fence) == (std::uint64_t{1} << 40),
          "equal signals are satisfied and reserved values fail without changing the counter");
    void *queried = nullptr;
    check(query(fence, &fence_interface, &queried) == 0 && queried == fence && release(queried) == 1 &&
              add(fence) == 2 && release(fence) == 1,
          "fence identity and reference methods preserve real ownership");
    check(release_device(device) == 1,
          "a live fence retains its parent after the guest releases its device reference");
    check(get_device(fence, &device11_interface, &queried) == 0 && queried == device &&
              release_device(queried) == 1,
          "GetDevice returns the retained real parent with a new guest reference");
    check(errno == E2BIG && get_last_error() == 71 && crt_error() == 19,
          "fence ABI methods preserve host and guest error state");
    check(release(fence) == 0, "final fence release frees its semaphore and last parent reference");
    device = nullptr;
    check(create_device(nullptr, nullptr, &device11_interface, &device) == 0 && device &&
              release_device(device) == 0,
          "fence-child teardown releases the device backend for actual recreation");
    errno = saved_errno;
}
