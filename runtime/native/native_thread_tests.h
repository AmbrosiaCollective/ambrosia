struct NativeThreadRecord {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool release = false;
    bool context_valid = false;
    bool exit_cleanup = false;
    bool alternate_stack = false;
    std::uint32_t id = 0;
    std::uint32_t initial_tls = 0;
    std::size_t stack_bytes = 0;
};

void AMBROSIA_GUEST_ABI native_thread_tls(void *, std::uint32_t, void *) {}

std::uint32_t AMBROSIA_GUEST_ABI native_thread_body(void *argument) {
    using namespace ambrosia;
    using namespace services;
    auto &record = *static_cast<NativeThreadRecord *>(argument);
    auto *graph = current_modules();
    const auto module = graph->loaded_module();
    const auto bounds = host::current_stack_bounds();
    record.stack_bytes = bounds.upper - bounds.lower;
    record.id = host::current_thread_id();
    record.context_valid = current_runtime() && guest::ThreadContext::current() && get_last_error() == 0;
    record.initial_tls = *graph_tls(reinterpret_cast<void *>(module));
    *graph_tls(reinterpret_cast<void *>(module)) = 55;
    guest::ThreadContext::current()->on_exit([&record, graph, module] {
        record.exit_cleanup = guest::ThreadContext::current() && current_modules() == graph &&
                              *graph_tls(reinterpret_cast<void *>(module)) == 55;
    });
#if defined(__linux__)
    stack_t stack{};
    record.alternate_stack =
        !sigaltstack(nullptr, &stack) && !(stack.ss_flags & SS_DISABLE) && stack.ss_size >= 64 * 1024;
#endif
    std::unique_lock lock(record.mutex);
    record.entered = true;
    record.changed.notify_all();
    record.changed.wait(lock, [&] { return record.release; });
    return 0x1234;
}

std::uint32_t AMBROSIA_GUEST_ABI native_thread_fast(void *argument) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(argument));
}

Bytes native_thread_fixture() {
    auto bytes = tls_fixture();
    text(bytes, 0x440, "kernel32.dll");
    text(bytes, 0x4a2, "GetLastError");
    jump_to_host(bytes, 0x200, reinterpret_cast<std::uintptr_t>(&native_thread_body));
    jump_to_host(bytes, 0x300, reinterpret_cast<std::uintptr_t>(&native_thread_tls));
    jump_to_host(bytes, 0x380, reinterpret_cast<std::uintptr_t>(&native_thread_fast));
    return bytes;
}

void native_thread_handle_checks() {
    using namespace ambrosia;
    using namespace services;
    using namespace std::chrono_literals;
    Runtime runtime;
    pe::ModuleGraph graph([&](const auto &symbol) { return runtime.resolve(symbol); });
    graph.add("thread.exe", native_thread_fixture());
    graph.load("thread.exe");
    Scope scope(runtime, &graph);
    auto process = graph.start();
    using Create =
        std::uintptr_t(AMBROSIA_GUEST_ABI *)(void *, std::size_t, ThreadEntry, void *, std::uint32_t, void *);
    using ExitCode = std::int32_t(AMBROSIA_GUEST_ABI *)(std::uintptr_t, void *);
    using ThreadId = std::uint32_t(AMBROSIA_GUEST_ABI *)(std::uintptr_t);
    const auto create =
        reinterpret_cast<Create>(runtime.resolve(service_symbol("CreateThread", "kernel32.dll")));
    const auto get_code =
        reinterpret_cast<ExitCode>(runtime.resolve(service_symbol("GetExitCodeThread", "kernel32.dll")));
    const auto get_id =
        reinterpret_cast<ThreadId>(runtime.resolve(service_symbol("GetThreadId", "kernel32.dll")));
    const auto entry = reinterpret_cast<ThreadEntry>(graph.loaded_module() + 0x1000);
    const auto fast = reinterpret_cast<ThreadEntry>(graph.loaded_module() + 0x1180);
    NativeThreadRecord record;
    std::array<std::byte, 10> id_guard;
    id_guard.fill(std::byte{0xcc});
    std::uintptr_t handle = 0;
    std::uint32_t reported_id = 0;
    bool parent_intact = false;
    const auto saved_error = errno;
    errno = EDOM;
    process.with_guest_context([&] {
        *graph_tls(reinterpret_cast<void *>(graph.loaded_module())) = 777;
        set_last_error(91);
        handle = create(nullptr, 65536, entry, &record, 0, id_guard.data() + 3);
        std::memcpy(&reported_id, id_guard.data() + 3, sizeof(reported_id));
        parent_intact =
            *graph_tls(reinterpret_cast<void *>(graph.loaded_module())) == 777 && get_last_error() == 91;
    });
    bool entered = false;
    {
        std::unique_lock lock(record.mutex);
        entered = record.changed.wait_for(lock, 2s, [&] { return record.entered; });
    }
    check(handle && reported_id && id_guard[2] == std::byte{0xcc} && id_guard[7] == std::byte{0xcc} &&
              entered && record.id == reported_id && get_id(handle) == reported_id,
          "CreateThread publishes a real native ID through exactly four unaligned bytes and an opaque "
          "waitable handle");
    check(record.context_valid && record.initial_tls == 41 && record.stack_bytes >= 65536 &&
              record.alternate_stack && parent_intact && errno == EDOM,
          "the original PE worker receives its argument, fresh guest TLS/errors, bounded native stack and "
          "alternate fault stack");
    std::uint32_t code = 0;
    const auto active_status = get_code(handle, &code);
    check(active_status == 1 && code == 259 && runtime.objects().wait(handle, 0).result == wait_timeout,
          "running thread queries return STILL_ACTIVE and an unsignaled wait object");
    check(runtime.objects().set_event(handle) == 6 && !get_code(handle, nullptr) && get_last_error() == 87,
          "thread objects reject event mutation and missing exit-code output");
    {
        std::lock_guard lock(record.mutex);
        record.release = true;
    }
    record.changed.notify_all();
    const auto completed = runtime.objects().wait(handle, 2000);
    const auto completed_status = get_code(handle, &code);
    check(completed.result == wait_object_0 && completed_status == 1 && code == 0x1234 &&
              record.exit_cleanup && runtime.objects().wait(handle, 0).result == wait_object_0,
          "thread completion waits remain signaled and publish the real DWORD exit code after TLS cleanup");
    (void)runtime.objects().close(handle);
    check(!get_code(handle, &code) && get_last_error() == 6 && get_id(handle) == 0,
          "closing a thread handle invalidates subsequent metadata queries");
    NativeThreadRecord detached;
    const auto detached_handle = create(nullptr, 0, entry, &detached, 0, nullptr);
    const auto closed = runtime.objects().close(detached_handle);
    {
        std::lock_guard lock(detached.mutex);
        detached.release = true;
    }
    detached.changed.notify_all();
    bool repeated = true;
    for (unsigned index = 0; index < 80; ++index) {
        const auto next = create(nullptr, 65536, fast, reinterpret_cast<void *>(23), 0, nullptr);
        const auto waited = runtime.objects().wait(next, 2000);
        const auto queried = get_code(next, &code);
        const auto released = runtime.objects().close(next);
        repeated =
            repeated && next && waited.result == wait_object_0 && queried == 1 && code == 23 && !released;
    }
    check(repeated,
          "finished native jobs are reaped so sequential creations exceed the live-job budget without leaks");
    const auto null_entry = create(nullptr, 0, nullptr, nullptr, 0, nullptr);
    const auto null_error = get_last_error();
    const auto oversized = create(nullptr, NativeThreads::stack_limit + 1, fast, nullptr, 0, nullptr);
    check(!null_entry && null_error == 87 && !oversized && get_last_error() == 8,
          "null entries and over-budget native stack requests fail without publishing a thread");
    runtime.threads().shutdown();
    check(detached_handle && !closed && detached.exit_cleanup,
          "closing a handle does not cancel its worker; shutdown joins and retains its guest cleanup state");
    const auto after_shutdown = create(nullptr, 0, fast, nullptr, 0, nullptr);
    check(!after_shutdown && get_last_error() == 995,
          "a shut-down thread directory rejects new work instead of starting a detached guest");
    process.close();
    errno = saved_error;
}

NativeThreadRecord *cli_thread_record = nullptr;
bool cli_thread_created = false;

std::uint64_t AMBROSIA_GUEST_ABI native_thread_parent() {
    using namespace ambrosia::services;
    using Create =
        std::uintptr_t(AMBROSIA_GUEST_ABI *)(void *, std::size_t, ThreadEntry, void *, std::uint32_t, void *);
    auto *runtime = current_runtime();
    const auto create =
        reinterpret_cast<Create>(runtime->resolve(service_symbol("CreateThread", "kernel32.dll")));
    const auto entry = reinterpret_cast<ThreadEntry>(current_modules()->loaded_module() + 0x1180);
    const auto handle = create(nullptr, 65536, entry, cli_thread_record, 0, nullptr);
    cli_thread_created = handle != 0;
    {
        std::lock_guard lock(cli_thread_record->mutex);
        cli_thread_record->release = true;
    }
    cli_thread_record->changed.notify_all();
    return 77;
}

void native_thread_launcher_checks() {
    using namespace ambrosia;
    NativeThreadRecord record;
    auto bytes = native_thread_fixture();
    jump_to_host(bytes, 0x200, reinterpret_cast<std::uintptr_t>(&native_thread_parent));
    jump_to_host(bytes, 0x380, reinterpret_cast<std::uintptr_t>(&native_thread_body));
    const auto root = std::filesystem::path("/work/native-thread-launcher");
    const auto package = root / "package";
    std::filesystem::create_directories(package);
    const auto image = package / "thread.exe";
    {
        std::ofstream file(image, std::ios::binary);
        file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file) {
            throw std::runtime_error("cannot write original thread launcher fixture");
        }
    }
    const std::array<std::filesystem::path, 6> arguments{"--run-bootstrap", image,    "--package",
                                                         package,           "--work", root / "cache"};
    cli_thread_record = &record;
    cli_thread_created = false;
    std::ostringstream output, errors;
    const auto code = run_native_command(arguments, output, errors);
    cli_thread_record = nullptr;
    check(code == 0 && errors.str().empty() &&
              output.str().find("\"bootstrap_result\":77") != std::string::npos && cli_thread_created &&
              record.context_valid && record.exit_cleanup,
          "the real native CLI joins guest workers and their TLS cleanup before process/module teardown");
}

std::atomic<unsigned> suspended_attach_calls{0};
std::atomic<unsigned> suspended_detach_calls{0};

void AMBROSIA_GUEST_ABI suspended_thread_tls(void *, std::uint32_t reason, void *) {
    if (reason == 2) {
        ++suspended_attach_calls;
    } else if (reason == 3) {
        ++suspended_detach_calls;
    }
}

void native_suspended_thread_checks() {
    using namespace ambrosia;
    using namespace services;
    using namespace std::chrono_literals;
    Runtime runtime;
    auto bytes = native_thread_fixture();
    jump_to_host(bytes, 0x300, reinterpret_cast<std::uintptr_t>(&suspended_thread_tls));
    pe::ModuleGraph graph([&](const auto &symbol) { return runtime.resolve(symbol); });
    graph.add("suspended.exe", bytes);
    graph.load("suspended.exe");
    Scope scope(runtime, &graph);
    auto process = graph.start();
    using Create =
        std::uintptr_t(AMBROSIA_GUEST_ABI *)(void *, std::size_t, ThreadEntry, void *, std::uint32_t, void *);
    using Resume = std::uint32_t(AMBROSIA_GUEST_ABI *)(std::uintptr_t);
    const auto create =
        reinterpret_cast<Create>(runtime.resolve(service_symbol("CreateThread", "kernel32.dll")));
    const auto resume =
        reinterpret_cast<Resume>(runtime.resolve(service_symbol("ResumeThread", "kernel32.dll")));
    const auto entry = reinterpret_cast<ThreadEntry>(graph.loaded_module() + 0x1000);
    NativeThreadRecord record;
    suspended_attach_calls = suspended_detach_calls = 0;
    std::uint32_t id = 0;
    const auto handle = create(nullptr, 358400, entry, &record, 4, &id);
    bool entered_before_resume;
    {
        std::unique_lock lock(record.mutex);
        entered_before_resume = record.changed.wait_for(lock, 100ms, [&] { return record.entered; });
    }
    std::uint32_t code = 0;
    const auto status_error = runtime.threads().exit_code(handle, &code);
    check(handle && id && !entered_before_resume && suspended_attach_calls == 0 &&
              suspended_detach_calls == 0 && !status_error && code == 259 &&
              runtime.objects().wait(handle, 0).result == wait_timeout,
          "CREATE_SUSPENDED creates a real unsignaled thread but does not run guest TLS/module callbacks or "
          "entry before resume");
    const auto saved_error = errno;
    errno = EDOM;
    set_last_error(91);
    const auto previous = resume(handle);
    const auto repeated = resume(handle);
    check(previous == 1 && repeated == 0 && errno == EDOM && get_last_error() == 91,
          "ResumeThread releases the initial suspension once, reports the previous count and preserves "
          "success error state");
    bool entered_after_resume;
    {
        std::unique_lock lock(record.mutex);
        entered_after_resume = record.changed.wait_for(lock, 2s, [&] { return record.entered; });
        record.release = true;
    }
    record.changed.notify_all();
    const auto done = runtime.objects().wait(handle, 2000);
    const auto code_error = runtime.threads().exit_code(handle, &code);
    check(entered_after_resume && done.result == wait_object_0 && !code_error && code == 0x1234 &&
              record.exit_cleanup && suspended_attach_calls == 1 && suspended_detach_calls == 1,
          "resuming dispatches guest attach, original PE entry and detach/TLS cleanup exactly once with the "
          "real exit code");
    (void)runtime.objects().close(handle);
    const auto stale_resume = resume(handle);
    const auto stale_error = get_last_error();
    const auto event = runtime.objects().create_event(true, false);
    const auto event_resume = resume(event.handle);
    check(stale_resume == 0xffffffffu && stale_error == 6 && event_resume == 0xffffffffu &&
              get_last_error() == 6,
          "ResumeThread rejects stale and non-thread handles without changing another object's state");
    (void)runtime.objects().close(event.handle);
    runtime.threads().shutdown();
    process.close();
    errno = saved_error;
}
