void native_thread_priority_checks() {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    pe::ModuleGraph graph([&](const auto &symbol) { return runtime.resolve(symbol); });
    graph.add("priority.exe", native_thread_fixture());
    graph.load("priority.exe");
    Scope scope(runtime, &graph);
    auto process = graph.start();
    using Create =
        std::uintptr_t(AMBROSIA_GUEST_ABI *)(void *, std::size_t, ThreadEntry, void *, std::uint32_t, void *);
    using SetPriority = std::int32_t(AMBROSIA_GUEST_ABI *)(std::uintptr_t, std::int32_t);
    using GetPriority = std::int32_t(AMBROSIA_GUEST_ABI *)(std::uintptr_t);
    const auto create =
        reinterpret_cast<Create>(runtime.resolve(service_symbol("CreateThread", "kernel32.dll")));
    const auto set =
        reinterpret_cast<SetPriority>(runtime.resolve(service_symbol("SetThreadPriority", "kernel32.dll")));
    const auto get =
        reinterpret_cast<GetPriority>(runtime.resolve(service_symbol("GetThreadPriority", "kernel32.dll")));
    const auto fast = reinterpret_cast<ThreadEntry>(graph.loaded_module() + 0x1180);
    std::uint32_t id = 0;
    const auto handle = create(nullptr, 65536, fast, reinterpret_cast<void *>(23), 4, &id);
    const auto baseline = host::thread_niceness(id);
    const auto owner_before = host::thread_niceness(host::current_thread_id());
    check(handle && id && baseline.error == host::SchedulingError::none && get(handle) == 0,
          "new owned thread priority starts normal with an observed SCHED_OTHER native baseline");
    const auto saved_error = errno;
    errno = EDOM;
    crt_error() = 61;
    set_last_error(91);
    const auto normal = set(handle, 0);
    const auto unchanged = host::thread_niceness(id);
    check(normal == 1 && get(handle) == 0 && unchanged.error == host::SchedulingError::none &&
              unchanged.value == baseline.value && errno == EDOM && crt_error() == 61 &&
              get_last_error() == 91,
          "normal priority confirms the native baseline and preserves successful caller error state");
    const auto rejected = set(handle, 3);
    const auto rejected_error = get_last_error();
    check(rejected == 0 && rejected_error == 87 && get(handle) == 0 &&
              host::thread_niceness(id).value == baseline.value,
          "invalid Windows priority levels fail transactionally without a host scheduling change");
    const auto raised = set(handle, 2);
    const auto raise_error = get_last_error();
    const auto native_raised = host::thread_niceness(id);
    const auto target_raised = std::clamp(baseline.value - 10, -20, 19);
    check(
        native_raised.error == host::SchedulingError::none &&
            ((raised == 1 && get(handle) == 2 && native_raised.value == target_raised) ||
             (raised == 0 && raise_error == 5 && get(handle) == 0 && native_raised.value == baseline.value)),
        "highest priority either applies the real Linux nice request or returns host permission denial "
        "without inventing success/state");
    const auto lowered = set(handle, -1);
    const auto native_lowered = host::thread_niceness(id);
    check(lowered == 1 && get(handle) == -1 && native_lowered.error == host::SchedulingError::none &&
              native_lowered.value == std::clamp(baseline.value + 5, -20, 19),
          "below-normal priority changes only the owned worker's real Linux nice value and retained Windows "
          "level");
    const auto owner_after = host::thread_niceness(host::current_thread_id());
    check(owner_before.error == host::SchedulingError::none &&
              owner_after.error == host::SchedulingError::none && owner_before.value == owner_after.value &&
              errno == EDOM && crt_error() == 61,
          "worker scheduling changes leave the submitting host thread's nice value and error channels "
          "unchanged");
    const auto event = runtime.objects().create_event(true, false);
    const auto event_set = set(event.handle, 0);
    const auto event_error = get_last_error();
    const auto event_get = get(event.handle);
    check(event_set == 0 && event_error == 6 && event_get == 0x7fffffff && get_last_error() == 6,
          "priority APIs reject non-thread handles with Windows error sentinels");
    (void)runtime.objects().close(event.handle);
    const auto resumed = runtime.threads().resume(handle);
    const auto completed = runtime.objects().wait(handle, 2000);
    const auto ended_set = set(handle, 0);
    check(
        !resumed.error && resumed.previous == 1 && completed.result == wait_object_0 && ended_set == 0 &&
            get_last_error() == 6 && get(handle) == -1,
        "completed threads retain their last applied level but cannot schedule an exited/recycled native ID");
    (void)runtime.objects().close(handle);
    check(set(handle, 0) == 0 && get_last_error() == 6 && get(handle) == 0x7fffffff,
          "closed thread handles cannot reach host scheduling by stale ID");
    runtime.threads().shutdown();
    process.close();
    errno = saved_error;
}
