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
    using Heap = void *(AMBROSIA_GUEST_ABI *)(void *, void *, std::uint32_t, std::uint32_t);
    const auto heap = reinterpret_cast<Heap>(table[26]);

    struct GuardedHeap {
        std::uint64_t before = 0x1234567812345678ull;
        std::array<std::uint32_t, 5> fields{};
        std::uint32_t after = 0x87654321;
    } properties;

    auto *returned = heap(device, properties.fields.data(), 1, 1);
    const auto default_pool = properties.fields[2];
    check(returned == properties.fields.data() && properties.before == 0x1234567812345678ull &&
              properties.after == 0x87654321 && properties.fields[0] == 4 && properties.fields[1] == 1 &&
              (default_pool == 1 || default_pool == 2) && properties.fields[3] == 1 &&
              properties.fields[4] == 1,
          "Windows member struct-return ABI writes exactly the custom default-heap properties");
    returned = heap(device, properties.fields.data(), 0, 2);
    check(returned == properties.fields.data() &&
              properties.fields == std::array<std::uint32_t, 5>{4, 2, 1, 1, 1},
          "upload heaps map to CPU write-combined system memory for the actual one-node backend");
    returned = heap(device, properties.fields.data(), 1, 3);
    check(returned == properties.fields.data() &&
              properties.fields == std::array<std::uint32_t, 5>{4, 3, 1, 1, 1} &&
              properties.before == 0x1234567812345678ull && properties.after == 0x87654321 &&
              errno == E2BIG && get_last_error() == 73 && crt_error() == 29,
          "readback heap metadata preserves neighboring bytes and caller error state");

    struct HeapByValue {
        std::array<std::uint32_t, 5> fields;
    };

    using AggregateHeap = HeapByValue(AMBROSIA_GUEST_ABI *)(void *, std::uint32_t, std::uint32_t);
    const auto aggregate_heap = reinterpret_cast<AggregateHeap>(table[26]);
    const auto aggregate_result = aggregate_heap(device, 1, 2);
    check(aggregate_result.fields == std::array<std::uint32_t, 5>{4, 2, 1, 1, 1},
          "an independently compiled Microsoft aggregate return receives correct heap fields");
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
    check(create_fence(device, 7, probe == 3 ? 2 : 0, &fence_interface, &fence) == 0 && fence,
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
    void *shared = nullptr;
    check(create_fence(device, 3, 1, &fence_interface, &shared) == 0 && shared,
          "SHARED fence creation requires a real exportable host timeline semaphore");
    if (shared) {
        auto *shared_table = *static_cast<const std::uintptr_t **>(shared);
        const auto shared_completed = reinterpret_cast<Counter>(shared_table[8]);
        const auto shared_signal = reinterpret_cast<Signal>(shared_table[10]);
        const auto shared_release = reinterpret_cast<Reference>(shared_table[2]);
        check(shared_completed(shared) == 3 && shared_signal(shared, 11) == 0 &&
                  shared_completed(shared) == 11,
              "shared fence counter reads and host signals use the actual exportable semaphore");
        check(shared_release(shared) == 0,
              "final shared fence release retires its real semaphore and independent parent lease");
    }
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

void graphics_queue_live_checks(const std::filesystem::path &root, unsigned probe) {
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
          "command queue fixture owns a real device with a host queue");
    auto *device_table = *static_cast<const std::uintptr_t **>(device);
    using CreateQueue = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, const void *, const void *, void **);
    using Query = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, const void *, void **);
    using Reference = std::uint32_t(AMBROSIA_GUEST_ABI *)(void *);
    const auto create_queue = reinterpret_cast<CreateQueue>(device_table[8]);
    const auto release_device = reinterpret_cast<Reference>(device_table[2]);
    check(create_queue(device, reinterpret_cast<const void *>(1), reinterpret_cast<const void *>(1),
                       nullptr) == pointer_error,
          "CreateCommandQueue validates output before reading descriptor or identity");
    void *output = reinterpret_cast<void *>(1);
    check(create_queue(device, nullptr, &command_queue_interface, &output) == pointer_error && !output,
          "CreateCommandQueue clears output before rejecting a missing descriptor");
    const Guid unsupported{};
    const std::array<std::uint32_t, 4> description{};
    check(create_queue(device, description.data(), &unsupported, &output) == no_interface && !output,
          "CreateCommandQueue rejects an unsupported identity without frontend ownership");
    const auto create_queue_x = reinterpret_cast<CreateQueue>(device_table[48]);
    check(create_queue_x(device, reinterpret_cast<const void *>(1), reinterpret_cast<const void *>(1),
                         nullptr) == pointer_error,
          "CreateCommandQueueX validates output before reading the console descriptor");
    output = reinterpret_cast<void *>(1);
    check(create_queue_x(device, nullptr, &command_queue_interface, &output) == pointer_error && !output,
          "CreateCommandQueueX clears output before rejecting a missing descriptor");
    check(create_queue_x(device, description.data(), &unsupported, &output) == no_interface && !output,
          "CreateCommandQueueX rejects unknown interfaces through the same queue ownership path");
    std::array<void *, 8> queues{};
    const std::array<std::uint32_t, 8> types{0, 2, 3, 2, 2, 2, 2, 2};
    errno = E2BIG;
    set_last_error(79);
    crt_error() = 21;
    for (std::size_t index = 0; index < queues.size(); ++index) {
        std::uint32_t flags = 0;
        if (probe == 4) {
            flags = 2;
        } else if (index == 2) {
            flags = 1;
        }
        std::array<std::uint32_t, 4> selected{types[index], probe == 2 ? 100u : 0u, flags, 1};
        auto create_selected = create_queue;
        if (index >= 3) {
            create_selected = create_queue_x;
            const auto engine = static_cast<std::uint32_t>(index - 3);
            selected = {2, 0, engine, 0};
            if (index == queues.size() - 1) {
                selected = {2, 0, 0, 1};
            }
            if (probe == 5) {
                selected[1] = 1;
            } else if (probe == 6) {
                selected[2] = 4;
            } else if (probe == 7) {
                selected[3] = 2;
            }
        }
        check(create_selected(device, selected.data(), &command_queue_interface, &queues[index]) == 0 &&
                  queues[index],
              "PC queue kinds and bounded Xbox engine selectors acquire independent parent leases");
    }
    struct QueueFields {
        std::array<std::uint32_t, 4> fields;
    };
    static_assert(sizeof(QueueFields) == 16);
    using GetDescription = void *(AMBROSIA_GUEST_ABI *)(void *, void *);
    using AggregateDescription = QueueFields(AMBROSIA_GUEST_ABI *)(void *);
    auto *policy_table = *static_cast<const std::uintptr_t **>(queues[2]);
    const auto get_description = reinterpret_cast<GetDescription>(policy_table[18]);
    struct GuardedDescription {
        std::uint64_t before = 0x1234567812345678ull;
        QueueFields value{};
        std::uint64_t after = 0x8765432187654321ull;
    } description_output;
    check(get_description(queues[2], &description_output.value) == &description_output.value &&
              description_output.value.fields == std::array<std::uint32_t, 4>{3, 0, 1, 1} &&
              description_output.before == 0x1234567812345678ull &&
              description_output.after == 0x8765432187654321ull,
          "GetDesc returns the exact retained PC queue flags through the SDK C-style ABI");
    const auto aggregate_description = reinterpret_cast<AggregateDescription>(policy_table[18]);
    check(aggregate_description(queues[2]).fields == std::array<std::uint32_t, 4>{3, 0, 1, 1},
          "compiled Microsoft aggregate-return ABI receives all four queue fields");
    auto *console_table = *static_cast<const std::uintptr_t **>(queues.back());
    const auto console_description = reinterpret_cast<AggregateDescription>(console_table[18]);
    check(console_description(queues.back()).fields == std::array<std::uint32_t, 4>{2, 0, 0, 1},
          "Xbox engine selection normalizes to standard PC policy instead of becoming PC flags");
    auto *table = *static_cast<const std::uintptr_t **>(queues[0]);
    const auto query = reinterpret_cast<Query>(table[0]);
    const auto add = reinterpret_cast<Reference>(table[1]);
    const auto release = reinterpret_cast<Reference>(table[2]);
    const auto get_device = reinterpret_cast<Query>(table[7]);
    if (probe == 1) {
        reinterpret_cast<Reference>(table[10])(queues[0]);
        throw std::runtime_error("unimplemented queue execution unexpectedly returned");
    }
    if (probe == 3) {
        Runtime other;
        Scope foreign(other);
        (void)add(queues[0]);
        throw std::runtime_error("foreign-runtime queue unexpectedly accepted");
    }
    check(query(queues[0], &command_queue_interface, &output) == 0 && output == queues[0] &&
              release(output) == 1 && add(queues[0]) == 2 && release(queues[0]) == 1,
          "queue identity and reference counts share one actual owned frontend");
    check(release_device(device) == queues.size(),
          "PC and Xbox queue children retain their device independently");
    check(get_device(queues[0], &device11_interface, &output) == 0 && output == device &&
              release_device(output) == queues.size(),
          "queue GetDevice returns the retained backend parent with an added reference");
    check(errno == E2BIG && get_last_error() == 79 && crt_error() == 21,
          "queue facade preserves native and guest error state");
    for (auto *queue : queues) {
        auto *entries = *static_cast<const std::uintptr_t **>(queue);
        check(reinterpret_cast<Reference>(entries[2])(queue) == 0,
              "each final queue reference releases its own parent lease");
    }
    device = nullptr;
    check(create_device(nullptr, nullptr, &device11_interface, &device) == 0 && device &&
              release_device(device) == 0,
          "final queue-child teardown frees actual backend ownership for recreation");
}
