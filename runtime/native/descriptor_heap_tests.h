void descriptor_heap_checks(const std::filesystem::path &root, unsigned probe) {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    pe::ModuleGraph graph;
    graph.add("hooks.dll", title_hook_fixture());
    graph.load("hooks.dll");
    Scope scope(runtime, &graph);
    runtime.graphics_devices().bind(root);
    auto execution = graph.start();

    struct ResetObserver {
        ~ResetObserver() {
            title_hook_observer = {};
            title_allocation_mode = 0;
        }
    } reset;

    execution.with_guest_context([&] {
        void *device = nullptr;
        check(runtime.graphics_devices().create(&device11_interface, &device, std::nullopt) == 0 && device,
              "heap fixture owns an actual backend device in an initialized title context");
        auto *device_table = *static_cast<const std::uintptr_t **>(device);
        using Create = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, const void *, const void *, void **);
        using Reference = std::uint32_t(AMBROSIA_GUEST_ABI *)(void *);
        using Query = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, const void *, void **);
        using Handle = std::uint64_t(AMBROSIA_GUEST_ABI *)(void *);
        using Description =
            DescriptorHeapDescription *(AMBROSIA_GUEST_ABI *)(void *, DescriptorHeapDescription *);
        using Aggregate = DescriptorHeapDescription(AMBROSIA_GUEST_ABI *)(void *);
        const auto create = reinterpret_cast<Create>(device_table[14]);
        const auto add_device = reinterpret_cast<Reference>(device_table[1]);
        const auto release_device = reinterpret_cast<Reference>(device_table[2]);
        unsigned reentries = 0;
        title_hook_observer = [&] {
            check(add_device(device) >= 2 && release_device(device) >= 1,
                  "title allocation/free can reenter graphics without its ownership lock");
            ++reentries;
        };
        title_allocations = title_releases = 0;
        DescriptorHeapDescription description{0, 128, 1, 1};
        void *heap = reinterpret_cast<void *>(1);
        check(create(device, nullptr, &descriptor_heap_interface, &heap) == pointer_error && !heap &&
                  create(device, &description, nullptr, &heap) == pointer_error && !heap,
              "heap creation validates pointers and clears output before rejection");
        const Guid unknown{};
        check(create(device, &description, &unknown, &heap) == no_interface && !heap &&
                  create(device, &description, &descriptor_heap_interface, nullptr) == 1 &&
                  title_allocations == 0,
              "unknown identities and capability checks allocate no title storage");
        for (const auto invalid : {DescriptorHeapDescription{9, 1, 0, 0},
                                   {0, 0, 0, 0},
                                   {0, 1, 2, 0},
                                   {0, 1, 0, 2},
                                   {2, 1, 1, 0},
                                   {0, UINT32_MAX, 1, 0}}) {
            check(create(device, &invalid, &descriptor_heap_interface, &heap) ==
                          static_cast<std::int32_t>(0x80070057u) &&
                      !heap && title_allocations == 0,
                  "invalid heap type, count, visibility, node or extent fails before callbacks");
        }
        if (probe == 3) {
            title_allocation_mode = 1;
        }
        const auto saved_errno = errno;
        errno = E2BIG;
        set_last_error(89);
        crt_error() = 37;
        check(create(device, &description, &descriptor_heap_interface, &heap) == 0 && heap &&
                  title_requested_bytes == 128 * 32 &&
                  title_requested_attributes == descriptor_memory_attributes && errno == E2BIG &&
                  get_last_error() == 89 && crt_error() == 37,
              "heap creation uses the exact title allocator and preserves guest error state");
        auto *table = *static_cast<const std::uintptr_t **>(heap);
        const auto query = reinterpret_cast<Query>(table[0]);
        const auto add = reinterpret_cast<Reference>(table[1]);
        const auto release = reinterpret_cast<Reference>(table[2]);
        const auto get_device = reinterpret_cast<Query>(table[7]);
        if (probe == 1) {
            reinterpret_cast<Reference>(table[3])(heap);
            throw std::runtime_error("unsupported heap slot returned");
        }
        if (probe == 2) {
            Runtime other;
            Scope foreign(other, &graph);
            (void)add(heap);
            throw std::runtime_error("foreign heap owner returned");
        }
        void *output = reinterpret_cast<void *>(1);
        check(query(heap, &unknown, &output) == no_interface && !output &&
                  query(heap, &descriptor_heap_interface, &output) == 0 && output == heap &&
                  release(output) == 1 && add(heap) == 2 && release(heap) == 1,
              "heap interfaces and reference counts retain one owned identity");
        check(get_device(heap, &device11_interface, &output) == 0 && output == device &&
                  release_device(output) == 2,
              "heap GetDevice retains the actual parent");
        DescriptorHeapDescription explicit_result;
        check(reinterpret_cast<Description>(table[8])(heap, &explicit_result) == &explicit_result &&
                  explicit_result.type == 0 && explicit_result.count == 128 && explicit_result.flags == 1,
              "explicit-result GetDesc preserves the original heap description");
        const auto aggregate = reinterpret_cast<Aggregate>(table[8])(heap);
        const auto cpu = reinterpret_cast<Handle>(table[9])(heap);
        const auto gpu = reinterpret_cast<Handle>(table[10])(heap);
        check(aggregate.count == 128 && aggregate.node_mask == 1 && cpu == gpu &&
                  cpu == reinterpret_cast<std::uintptr_t>(title_last_address) &&
                  runtime.memory().writable_range(reinterpret_cast<void *>(cpu), 128 * 32),
              "aggregate GetDesc and scalar console handles preserve the title-selected storage");
        auto *records = reinterpret_cast<std::byte *>(cpu);
        records[0] = std::byte{0x51};
        records[127 * 32] = std::byte{0x73};
        check(records[0] == std::byte{0x51} && records[127 * 32] == std::byte{0x73},
              "descriptor handle arithmetic reaches actual writable title memory");
        check(release(heap) == 0 && title_releases == 1 && reentries == 2 &&
                  runtime.memory().reserved_bytes() == 0,
              "final heap release calls the paired title free outside the graphics lock");
        std::array<void *, 32> heaps{};
        description = {1, 16, 0, 0};
        for (auto &entry : heaps) {
            if (create(device, &description, &descriptor_heap_interface, &entry) != 0 || !entry) {
                throw std::runtime_error("bounded heap fixture allocation failed");
            }
        }
        const auto allocated_before = title_allocations;
        check(create(device, &description, &descriptor_heap_interface, &heap) ==
                      static_cast<std::int32_t>(0x8007000eu) &&
                  !heap && title_allocations == allocated_before,
              "the live/pending heap budget rejects before another title callback");
        auto *cpu_only_table = *static_cast<const std::uintptr_t **>(heaps[0]);
        check(reinterpret_cast<Handle>(cpu_only_table[9])(heaps[0]) != 0 &&
                  reinterpret_cast<Handle>(cpu_only_table[10])(heaps[0]) == 0,
              "CPU-only heaps do not publish a shader-visible GPU handle");
        for (auto *entry : heaps) {
            auto *entry_table = *static_cast<const std::uintptr_t **>(entry);
            (void)reinterpret_cast<Reference>(entry_table[2])(entry);
        }
        check(runtime.memory().reserved_bytes() == 0, "all bounded sampler heap allocations are reclaimed");
        description = {4, 8, 1, 0};
        check(create(device, &description, &descriptor_heap_interface, &heap) == 0 &&
                  title_requested_bytes == 8 * 32,
              "the console extra view-heap type retains the view descriptor stride");
        table = *static_cast<const std::uintptr_t **>(heap);
        check(release_device(device) == 1 && reinterpret_cast<Reference>(table[2])(heap) == 0 &&
                  runtime.memory().reserved_bytes() == 0,
              "a heap keeps its parent alive through final reentrant title free");
        title_hook_observer = {};
        errno = saved_errno;
    });
}
