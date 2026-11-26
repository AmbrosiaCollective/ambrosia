void affinity_mapping_checks() {
    using namespace ambrosia::services;
    ImportRegistry catalog;
    const auto target = reinterpret_cast<std::uintptr_t>(&sum6);
    for (unsigned index = 0; index < 65; ++index) {
        catalog.add("contract" + std::to_string(index) + ".dll", "probe", target);
    }
    NativeModules expanded(catalog);
    check(expanded.has_module("contract64.dll"),
          "native contract metadata can exceed64 modules while retaining lazy facade mapping budgets");
    for (unsigned index = 65; index < 128; ++index) {
        catalog.add("contract" + std::to_string(index) + ".dll", "probe", target);
    }
    check(runtime_rejected([&] { NativeModules over_budget(catalog); }),
          "the128-module metadata budget includes the reserved canonical native module");
    const std::array<unsigned, 2> duplicate{7, 7};
    const std::array<unsigned, 65> too_many{};
    check(runtime_rejected([&] { ThreadAffinity mapping(std::span<const unsigned>{}); }) &&
              runtime_rejected([&] { ThreadAffinity mapping(duplicate); }) &&
              runtime_rejected([&] { ThreadAffinity mapping(too_many); }),
          "affinity mappings reject empty, duplicate and over-one-group processor catalogs");
}

void native_thread_affinity_checks() {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    pe::ModuleGraph graph([&](const auto &symbol) { return runtime.resolve(symbol); });
    graph.add("affinity.exe", native_thread_fixture());
    graph.load("affinity.exe");
    Scope scope(runtime, &graph);
    auto process = graph.start();
    using Create =
        std::uintptr_t(AMBROSIA_GUEST_ABI *)(void *, std::size_t, ThreadEntry, void *, std::uint32_t, void *);
    using Get = std::int32_t(AMBROSIA_GUEST_ABI *)(std::uintptr_t, void *);
    using Set = std::int32_t(AMBROSIA_GUEST_ABI *)(std::uintptr_t, const void *, void *);
    using Mask = std::uint64_t(AMBROSIA_GUEST_ABI *)(std::uintptr_t, std::uint64_t);
    const auto create =
        reinterpret_cast<Create>(runtime.resolve(service_symbol("CreateThread", "kernel32.dll")));
    const auto get = reinterpret_cast<Get>(runtime.resolve(
        service_symbol("GetThreadGroupAffinity", "api-ms-win-core-processtopology-l1-1-0.dll")));
    const auto set =
        reinterpret_cast<Set>(runtime.resolve(service_symbol("SetThreadGroupAffinity", "kernel32.dll")));
    const auto mask =
        reinterpret_cast<Mask>(runtime.resolve(service_symbol("SetThreadAffinityMask", "kernelbase.dll")));
    const auto ids = runtime.processor_information().processor_ids();
    const auto fast = reinterpret_cast<ThreadEntry>(graph.loaded_module() + 0x1180);
    std::uint32_t id = 0;
    const auto handle = create(nullptr, 65536, fast, reinterpret_cast<void *>(23), 4, &id);
    const auto parent_before = host::thread_processors(host::current_thread_id());
    const auto before = host::thread_processors(id);
    std::array<std::byte, 24> guarded;
    guarded.fill(std::byte{0xcc});
    const auto saved_error = errno;
    errno = EDOM;
    crt_error() = 61;
    set_last_error(91);
    const auto queried = get(handle, guarded.data() + 3);
    GroupAffinity baseline;
    std::memcpy(&baseline, guarded.data() + 3, sizeof(baseline));
    const auto all_bits = UINT64_MAX >> (64 - ids.size());
    check(handle && id && queried == 1 && !baseline.group && baseline.mask == all_bits &&
              baseline.reserved == std::array<std::uint16_t, 3>{} && guarded[2] == std::byte{0xcc} &&
              guarded[19] == std::byte{0xcc} && before.error == host::SchedulingError::none &&
              std::equal(before.ids.begin(), before.ids.end(), ids.begin(), ids.end()),
          "group affinity reports the exact16-byte ABI with dense topology bits and the actual owned "
          "worker's native CPU set");
    check(errno == EDOM && crt_error() == 61 && get_last_error() == 91,
          "successful affinity queries preserve native errno and guest error channels");
    const GroupAffinity selected{std::uint64_t{1} << (ids.size() - 1), 0, {}};
    GroupAffinity previous;
    const auto applied = set(handle, &selected, &previous);
    const auto native_selected = host::thread_processors(id);
    GroupAffinity observed;
    const auto narrowed_query = get(handle, &observed);
    check(applied == 1 && previous.mask == baseline.mask && !previous.group && narrowed_query == 1 &&
              observed.mask == selected.mask && native_selected.error == host::SchedulingError::none &&
              native_selected.ids == std::vector<unsigned>{ids.back()},
          "setting a dense guest bit changes and verifies the corresponding native CPU, returning the old "
          "mask");
    std::vector<unsigned> reversed(ids.rbegin(), ids.rend());
    const ThreadAffinity reversed_mapping(reversed);
    GroupAffinity reversed_output;
    const auto reversed_error = reversed_mapping.read(id, reversed_output);
    check(!reversed_error && reversed_output.mask == 1 && !reversed_output.group,
          "affinity translation follows saved CPU identities even when their dense order differs from native "
          "IDs");
    std::array<GroupAffinity, 4> invalid{GroupAffinity{0, 0, {}}, GroupAffinity{selected.mask, 1, {}},
                                         GroupAffinity{selected.mask, 0, {1, 0, 0}},
                                         GroupAffinity{selected.mask, 0, {0, 0, 1}}};
    bool rejected = true;
    for (const auto &request : invalid) {
        guarded.fill(std::byte{0xcc});
        const auto result = set(handle, &request, guarded.data() + 3);
        rejected =
            rejected && result == 0 && get_last_error() == 87 &&
            std::all_of(guarded.begin(), guarded.end(), [](auto value) { return value == std::byte{0xcc}; });
    }
    if (ids.size() < 64) {
        const GroupAffinity outside{std::uint64_t{1} << ids.size(), 0, {}};
        const auto result = set(handle, &outside, nullptr);
        rejected = rejected && result == 0 && get_last_error() == 87;
    }
    check(rejected && host::thread_processors(id).ids == native_selected.ids,
          "empty/out-of-group/out-of-catalog/reserved masks fail without changing affinity or "
          "previous-output guards");
    GroupAffinity aliased{all_bits, 0, {}};
    const auto restored = set(handle, &aliased, &aliased);
    check(restored == 1 && aliased.mask == selected.mask && host::thread_processors(id).ids == before.ids,
          "input and previous-output buffers may alias because the request is copied before the old mask is "
          "written");
    const auto mask_previous = mask(handle, selected.mask);
    const auto mask_restored = mask(handle, all_bits);
    check(mask_previous == all_bits && mask_restored == selected.mask &&
              host::thread_processors(id).ids == before.ids,
          "SetThreadAffinityMask applies the same host-backed mapping and returns each real previous mask");
    GroupAffinity current;
    std::int32_t current_result = 0;
    process.with_guest_context([&] { current_result = get(static_cast<std::uintptr_t>(-2), &current); });
    const auto parent_after = host::thread_processors(host::current_thread_id());
    check(current_result == 1 && current.mask == all_bits && parent_before.ids == parent_after.ids,
          "the current-thread pseudo-handle resolves only under active guest TLS and child changes leave the "
          "caller's CPU set intact");
    const auto outside_context = get(static_cast<std::uintptr_t>(-2), &current);
    const auto context_error = get_last_error();
    const auto null_get = get(handle, nullptr);
    const auto null_get_error = get_last_error();
    const auto null_set = set(handle, nullptr, nullptr);
    check(!outside_context && context_error == 6 && !null_get && null_get_error == 87 && !null_set &&
              get_last_error() == 87,
          "missing guest context and missing affinity buffers return explicit Windows failures");
    const auto event = runtime.objects().create_event(true, false);
    const auto wrong_type = get(event.handle, &current);
    const auto wrong_error = get_last_error();
    (void)runtime.objects().close(event.handle);
    const auto resumed = runtime.threads().resume(handle);
    const auto completed = runtime.objects().wait(handle, 2000);
    const auto ended = get(handle, &current);
    check(!wrong_type && wrong_error == 6 && !resumed.error && completed.result == wait_object_0 && !ended &&
              get_last_error() == 6,
          "non-thread and completed handles never reach a foreign or recycled native CPU-affinity target");
    (void)runtime.objects().close(handle);
    const auto stale = mask(handle, selected.mask);
    check(!stale && get_last_error() == 6, "closed affinity handles are rejected before host scheduling");
    runtime.threads().shutdown();
    process.close();
    errno = saved_error;
}
