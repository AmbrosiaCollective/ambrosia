void process_priority_boost_checks() {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    using SetBoost = std::int32_t(AMBROSIA_GUEST_ABI *)(std::uintptr_t, std::int32_t);
    const auto address = runtime.resolve(service_symbol("SetProcessPriorityBoost", "kernel32.dll"));
    const auto set = reinterpret_cast<SetBoost>(address);
    check(address && runtime.resolve(service_symbol("SetProcessPriorityBoost",
                                                    "api-ms-win-core-processthreads-l1-1-2.dll")) == address,
          "the current process-priority boost contract resolves the same typed service as kernel32");
    const auto saved_error = errno;
    errno = EDOM;
    crt_error() = 61;
    set_last_error(91);
    const auto denied_disable = set(static_cast<std::uintptr_t>(-1), 1);
    const auto disable_error = get_last_error();
    const auto denied_enable = set(static_cast<std::uintptr_t>(-1), 0);
    const auto enable_error = get_last_error();
    const auto denied_nonzero = set(static_cast<std::uintptr_t>(-1), -1);
    check(!denied_disable && disable_error == 50 && !denied_enable && enable_error == 50 && !denied_nonzero &&
              get_last_error() == 50 && errno == EDOM && crt_error() == 61,
          "both boost directions report ERROR_NOT_SUPPORTED without fabricating scheduler changes or "
          "disturbing native and CRT errors");
    const auto event = runtime.objects().create_event(true, false);
    const auto null_target = set(0, 1);
    const auto null_error = get_last_error();
    const auto wrong_type = set(event.handle, 1);
    const auto wrong_error = get_last_error();
    const auto thread_pseudo = set(static_cast<std::uintptr_t>(-2), 1);
    check(
        !null_target && null_error == 6 && !wrong_type && wrong_error == 6 && !thread_pseudo &&
            get_last_error() == 6 && runtime.objects().wait(event.handle, 0).result == wait_timeout,
        "boost requests reject null, non-process and thread-pseudo targets without modifying another object");
    (void)runtime.objects().close(event.handle);
    auto bytes = fixture();
    text(bytes, 0x440, "kernel32.dll");
    text(bytes, 0x4a2, "SetProcessPriorityBoost");
    const unsigned char code[] = {0x48, 0x83, 0xec, 0x28, 0x48, 0xc7, 0xc1, 0xff, 0xff,
                                  0xff, 0xff, 0xba, 1,    0,    0,    0,    0xff, 0x15,
                                  0,    0,    0,    0,    0x48, 0x83, 0xc4, 0x28, 0xc3};
    std::memcpy(bytes.data() + 0x200, code, sizeof(code));
    put<std::uint32_t>(bytes, 0x212, 0x2080 - 0x1016);
    auto image = pe::Image::load(bytes, [&](const auto &symbol) { return runtime.resolve(symbol); });
    check(image.run_bootstrap() == 0 && get_last_error() == 50,
          "original PE entry passes full-width process identity and receives the real unsupported BOOL and "
          "LastError result");
    errno = saved_error;
}

void thread_priority_boost_checks() {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    pe::ModuleGraph graph([&](const auto &symbol) { return runtime.resolve(symbol); });
    graph.add("boost.exe", native_thread_fixture());
    graph.load("boost.exe");
    Scope scope(runtime, &graph);
    auto process = graph.start();
    using SetBoost = std::int32_t(AMBROSIA_GUEST_ABI *)(std::uintptr_t, std::int32_t);
    const auto set =
        reinterpret_cast<SetBoost>(runtime.resolve(service_symbol("SetThreadPriorityBoost", "kernel32.dll")));
    const auto entry = reinterpret_cast<ThreadEntry>(graph.loaded_module() + 0x1180);
    const auto worker =
        runtime.threads().create(runtime, graph, entry, reinterpret_cast<void *>(23), 65536, true);
    const auto baseline = host::thread_niceness(worker.id);
    const auto saved_error = errno;
    errno = EDOM;
    crt_error() = 61;
    const auto disabled = set(worker.handle, 1);
    const auto disable_error = get_last_error();
    const auto enabled = set(worker.handle, 0);
    const auto native = host::thread_niceness(worker.id);
    check(worker.handle && !disabled && disable_error == 50 && !enabled && get_last_error() == 50 &&
              baseline.error == host::SchedulingError::none && native.error == host::SchedulingError::none &&
              baseline.value == native.value && errno == EDOM && crt_error() == 61,
          "owned thread boost requests decline the unavailable capability and leave real nice state and "
          "caller errors intact");
    const auto query = runtime.threads().open(worker.id, 0x40, false);
    const auto denied_query = set(query.handle, 1);
    const auto query_error = get_last_error();
    const auto event = runtime.objects().create_event(true, false);
    const auto wrong_type = set(event.handle, 1);
    check(query.handle && !denied_query && query_error == 5 && !wrong_type && get_last_error() == 6,
          "thread boost requests enforce SET rights and reject unrelated kernel object types before "
          "capability handling");
    const auto outside = set(static_cast<std::uintptr_t>(-2), 1);
    const auto outside_error = get_last_error();
    bool active_declined = false;
    process.with_guest_context(
        [&] { active_declined = !set(static_cast<std::uintptr_t>(-2), 1) && get_last_error() == 50; });
    check(!outside && outside_error == 6 && active_declined,
          "the current-thread pseudo target needs a live guest context and never reports a successful boost "
          "change");
    const auto resumed = runtime.threads().resume(worker.handle);
    const auto waited = runtime.objects().wait(worker.handle, 2000);
    const auto ended = set(worker.handle, 1);
    const auto ended_error = get_last_error();
    (void)runtime.objects().close(query.handle);
    (void)runtime.objects().close(event.handle);
    (void)runtime.objects().close(worker.handle);
    const auto closed = set(worker.handle, 1);
    check(!resumed.error && waited.result == wait_object_0 && !ended && ended_error == 6 && !closed &&
              get_last_error() == 6,
          "finished and closed thread IDs cannot reach boost capability handling through stale handles");
    runtime.threads().shutdown();
    process.close();
    errno = saved_error;
}
