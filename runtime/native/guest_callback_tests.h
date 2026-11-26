void guest_callback_scope_checks() {
    using namespace ambrosia;
    using namespace services;
    using namespace std::chrono_literals;
    Runtime runtime;
    check(runtime_rejected([&] { (void)runtime.capture_callback([] {}); }),
          "guest callback capture requires its owning runtime and module scope");
    auto bytes = tls_fixture();
    put<std::uint32_t>(bytes, 0x98 + 120, 0);
    put<std::uint32_t>(bytes, 0x98 + 124, 0);
    jump_to_host(bytes, 0x300, reinterpret_cast<std::uintptr_t>(&worker_module_callback));
    pe::ModuleGraph graph;
    graph.add("callback.exe", bytes);
    graph.load("callback.exe");
    Scope scope(runtime, &graph);
    check(runtime_rejected([&] { (void)runtime.capture_callback([] {}); }),
          "metadata inspection cannot capture an executable guest callback context");
    worker_module_events.clear();
    auto process = graph.start();
    const auto base = graph.loaded_module();
    const auto owner_thread = std::this_thread::get_id();
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
    std::atomic<bool> captured_scope = false;
    std::atomic<bool> cleanup_scope = false;
    std::function<void()> callback;
    process.with_guest_context([&] {
        *graph_tls(reinterpret_cast<void *>(base)) = 444;
        set_last_error(91);
        callback = runtime.capture_callback([&] {
            auto *thread = guest::ThreadContext::current();
            captured_scope = thread && current_runtime() == &runtime && current_modules() != &graph &&
                             current_modules()->loaded_module() == base &&
                             std::this_thread::get_id() != owner_thread &&
                             *graph_tls(reinterpret_cast<void *>(base)) == 41 && get_last_error() == 0;
            if (!captured_scope) {
                const std::lock_guard lock(mutex);
                entered = true;
                changed.notify_all();
                return;
            }
            *graph_tls(reinterpret_cast<void *>(base)) = 42;
            set_last_error(61);
            thread->on_exit([&] {
                cleanup_scope = guest::ThreadContext::current() && current_runtime() == &runtime &&
                                current_modules()->loaded_module() == base && get_last_error() == 61 &&
                                *graph_tls(reinterpret_cast<void *>(base)) == 42;
            });
            std::unique_lock lock(mutex);
            entered = true;
            changed.notify_all();
            changed.wait(lock, [&] { return released; });
        });
    });
    check(runtime_rejected([&] { callback(); }),
          "a captured callback refuses inline substitution on the process owner thread");
    std::uintptr_t queue = 0;
    (void)runtime.task_queues().create(1, 1, &queue);
    const auto scheduler = runtime.task_queues().thread_pool_scheduler(queue);
    const auto owner = std::make_shared<int>(1);
    const auto accepted = scheduler(0, owner, callback);
    bool observed = false;
    {
        std::unique_lock lock(mutex);
        observed = changed.wait_for(lock, 2s, [&] { return entered; });
    }
    // Release an in-flight guest callback while owner shutdown is joining it.
    std::thread releaser([&] {
        std::this_thread::sleep_for(20ms);
        const std::lock_guard lock(mutex);
        released = true;
        changed.notify_all();
    });
    runtime.shutdown_workers();
    releaser.join();
    check(accepted && observed && captured_scope && cleanup_scope && !runtime.task_queues().workers(),
          "native queue callbacks own fresh module TLS, scoped catalog access and exit cleanup through join");
    // Keep the fixture safe even when a shutdown-order negative control fails.
    runtime.task_queues().shutdown();
    bool parent_intact = false;
    process.with_guest_context(
        [&] { parent_intact = *graph_tls(reinterpret_cast<void *>(base)) == 444 && get_last_error() == 91; });
    check(parent_intact, "callback TLS and last-error changes leave the process owner state intact");
    std::uintptr_t rejected = 99;
    check(!scheduler(0, owner, callback) &&
              runtime.task_queues().create(0, 0, &rejected) == gdk_out_of_memory && !rejected,
          "terminal worker shutdown rejects timers and new manual queues without publishing a handle");
    process.close();
    std::vector<std::pair<std::uint32_t, std::uint32_t>> worker_events;
    for (const auto &event : worker_module_events) {
        if (event.thread != owner_thread) {
            worker_events.emplace_back(event.value, event.reason);
        }
    }
    check(worker_events == std::vector<std::pair<std::uint32_t, std::uint32_t>>{{41, 2}, {42, 3}},
          "a queued callback attaches and detaches original PE TLS before process teardown");
    runtime.shutdown_workers();
}
