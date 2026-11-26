struct WorkerModuleEvent {
    std::thread::id thread;
    std::uint32_t value;
    std::uint32_t reason;
};

std::mutex worker_module_events_mutex;
std::vector<WorkerModuleEvent> worker_module_events;

void AMBROSIA_GUEST_ABI worker_module_callback(void *module, std::uint32_t reason, void *) {
    std::lock_guard lock(worker_module_events_mutex);
    worker_module_events.push_back({std::this_thread::get_id(), graph_tls(module)[0], reason});
}

void worker_module_checks() {
    using namespace ambrosia;
    using namespace services;
    using namespace std::chrono_literals;
    auto dependency = tls_fixture(true);
    put<std::uint32_t>(dependency, 0x98 + 16, 0);
    put<std::uint32_t>(dependency, 0x98 + 120, 0);
    put<std::uint32_t>(dependency, 0x98 + 124, 0);
    put<std::uint32_t>(dependency, 0x560, 100);
    jump_to_host(dependency, 0x300, reinterpret_cast<std::uintptr_t>(&worker_module_callback));
    constexpr unsigned char code[] = {0x8b, 0x0d, 0,    0, 0,    0, 0x65, 0x48, 0x8b,
                                      0x04, 0x25, 0x58, 0, 0,    0, 0x48, 0x8b, 0x04,
                                      0xc8, 0x83, 0,    1, 0x8b, 0, 0xc3};
    std::memcpy(dependency.data() + 0x200, code, sizeof(code));
    put<std::uint32_t>(dependency, 0x202, 0x2170 - 0x1006);
    dependency = exported_fixture(std::move(dependency), {{"named_value", {}, 0x1000}}, 77);
    auto root = tls_fixture();
    text(root, 0x440, "provider.dll");
    text(root, 0x4a2, "named_value");
    jump_to_host(root, 0x300, reinterpret_cast<std::uintptr_t>(&worker_module_callback));
    Runtime runtime;
    pe::ModuleGraph graph;
    graph.add("app.exe", root, "/synthetic/app.exe");
    graph.add("provider.dll", dependency, "/synthetic/provider.dll");
    graph.load("app.exe");
    check(runtime_rejected([&] { (void)graph.thread_snapshot(); }),
          "thread snapshots reject a loaded graph before process initialization completes");
    Scope scope(runtime, &graph);
    worker_module_events.clear();
    auto process = graph.start();
    const auto snapshot = graph.thread_snapshot();
    check(runtime_rejected([&] { (void)snapshot.run([] { return 0; }); }),
          "worker snapshots cannot substitute an inline call on the process owner thread");
    const auto provider = graph.loaded_module("provider.dll");
    const auto app = graph.loaded_module();
    process.with_guest_context([&] {
        *graph_tls(reinterpret_cast<void *>(provider)) = 555;
        *graph_tls(reinterpret_cast<void *>(app)) = 444;
        set_last_error(91);
    });
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
    bool unregistered_rejected = false;
    bool read_catalog = false;
    bool mutation_rejected = false;
    bool independent_errors = false;
    bool exit_context = false;
    bool worker_failed = false;
    std::uint32_t result = 0;
    bool cleared_after = false;
    std::thread worker;
    process.with_guest_context([&] {
        worker = std::thread([&] {
            try {
                unregistered_rejected = runtime_rejected([&] { (void)graph.loaded_module(); });
                Scope worker_scope(runtime, &graph);
                result = snapshot.run([&] {
                    read_catalog = graph.loaded_module("provider.dll") == provider &&
                                   graph.module_base_for_address(provider + 0x1000) == provider &&
                                   graph.loaded_module_path(provider) ==
                                       std::filesystem::path("/synthetic/provider.dll");
                    mutation_rejected = runtime_rejected(
                        [&] { (void)graph.export_address("provider.dll", named_symbol("named_value")); });
                    independent_errors = get_last_error() == 0;
                    set_last_error(27);
                    guest::ThreadContext::current()->on_exit([&] {
                        exit_context = guest::ThreadContext::current() && get_last_error() == 27 &&
                                       graph.loaded_module("provider.dll") == provider;
                    });
                    const auto address = graph.loaded_export(provider, named_symbol("named_value"));
                    const auto value = reinterpret_cast<std::uint32_t(AMBROSIA_GUEST_ABI *)()>(*address)();
                    {
                        std::unique_lock lock(mutex);
                        entered = true;
                        changed.notify_all();
                        changed.wait(lock, [&] { return released; });
                    }
                    return value;
                });
                cleared_after = host::guest_segment_base() == 0;
            } catch (...) {
                worker_failed = true;
            }
        });
    });
    const auto submitted = worker.joinable();
    bool observed_entry = false;
    {
        std::unique_lock lock(mutex);
        observed_entry = changed.wait_for(lock, 2s, [&] { return entered; });
    }
    bool premature_close_rejected = false;
    if (observed_entry) {
        premature_close_rejected = runtime_rejected([&] { process.close(); });
    }
    {
        std::lock_guard lock(mutex);
        released = true;
    }
    changed.notify_all();
    worker.join();
    check(submitted && observed_entry && !worker_failed && result == 101 && independent_errors &&
              cleared_after,
          "a real worker executes original PE code with fresh module TLS rather than the parent's modified "
          "block");
    check(unregistered_rejected && read_catalog && mutation_rejected,
          "catalog access is granted only within a registered worker; activation remains owner-only");
    check(premature_close_rejected && exit_context, "process detach refuses live workers and guest exit "
                                                    "callbacks retain TLS/catalog access through cleanup");
    bool parent_intact = false;
    process.with_guest_context([&] {
        parent_intact = *graph_tls(reinterpret_cast<void *>(provider)) == 555 &&
                        *graph_tls(reinterpret_cast<void *>(app)) == 444 && get_last_error() == 91;
    });
    check(parent_intact, "worker TLS and last-error updates leave the process owner's context unchanged");
    process.close();
    const auto owner = std::this_thread::get_id();
    std::vector<std::pair<std::uint32_t, std::uint32_t>> worker_events;
    for (const auto &event : worker_module_events) {
        if (event.thread != owner) {
            worker_events.emplace_back(event.value, event.reason);
        }
    }
    check(worker_events ==
              std::vector<std::pair<std::uint32_t, std::uint32_t>>{{100, 2}, {41, 2}, {41, 3}, {101, 3}},
          "worker TLS notifications attach in dependency order and detach in reverse order with final TLS "
          "values");
    auto next_process = graph.start();
    const auto fresh = graph.thread_snapshot();
    bool stale_rejected = false;
    std::uint32_t fresh_result = 0;
    host::WorkerPool next_pool;
    next_pool.start();
    const auto stale_submitted = next_pool.submit(
        [&] { stale_rejected = runtime_rejected([&] { (void)snapshot.run([] { return 0; }); }); });
    const auto fresh_submitted = next_pool.submit([&] {
        Scope worker_scope(runtime, &graph);
        fresh_result = fresh.run([&] {
            const auto address = graph.loaded_export(provider, named_symbol("named_value"));
            return reinterpret_cast<std::uint32_t(AMBROSIA_GUEST_ABI *)()>(*address)();
        });
    });
    next_pool.shutdown();
    check(stale_submitted && fresh_submitted && stale_rejected && fresh_result == 101,
          "old snapshots cannot enter a restarted process generation; new workers still get fresh TLS "
          "templates");
    next_process.close();

    pe::ModuleGraph disabled_graph;
    auto plain = provider_fixture();
    put<std::uint32_t>(plain, 0x98 + 16, 0x1000);
    jump_to_host(plain, 0x200, reinterpret_cast<std::uintptr_t>(&loader_dll_entry));
    disabled_graph.add("disabled.dll", plain);
    disabled_graph.load("disabled.dll");
    Scope disabled_scope(runtime, &disabled_graph);
    loader_reasons.clear();
    auto disabled_process = disabled_graph.start();
    const auto disabled_snapshot = disabled_graph.thread_snapshot();
    host::WorkerPool disabled_pool;
    disabled_pool.start();
    std::uint32_t disabled_result = 0;
    const auto disabled_submitted = disabled_pool.submit([&] {
        Scope worker_scope(runtime, &disabled_graph);
        disabled_result = disabled_snapshot.run([] { return 7; });
    });
    disabled_pool.shutdown();
    disabled_process.close();
    check(disabled_submitted && disabled_result == 7 && loader_reasons == std::vector<std::uint32_t>({1, 0}),
          "DisableThreadLibraryCalls suppresses real worker callouts while retaining process attach/detach");
}
