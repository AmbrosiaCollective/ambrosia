void drain_progress_queue(ambrosia::services::TaskQueues &queues,
                          const ambrosia::services::NativeQueueScheduler &scheduler) {
    using namespace std::chrono_literals;
    std::mutex mutex;
    std::condition_variable changed;
    unsigned entered = 0;
    unsigned finished = 0;
    bool released = false;
    bool accepted = true;
    const auto workers = queues.workers();
    const auto owner = std::make_shared<int>(1);
    for (unsigned index = 0; index < workers; ++index) {
        accepted = scheduler(0, owner,
                             [&] {
                                 std::unique_lock lock(mutex);
                                 ++entered;
                                 changed.notify_all();
                                 changed.wait(lock, [&] { return released; });
                                 ++finished;
                                 changed.notify_all();
                             }) &&
                   accepted;
    }
    std::unique_lock lock(mutex);
    const auto all_entered = changed.wait_for(lock, 2s, [&] { return entered == workers; });
    released = true;
    changed.notify_all();
    const auto all_finished = changed.wait_for(lock, 2s, [&] { return finished == workers; });
    lock.unlock();
    if (!accepted || !all_entered || !all_finished) {
        queues.shutdown();
        throw std::runtime_error("original progress queue fence did not complete");
    }
}

struct PackageProgressRecord {
    ambrosia::services::Runtime *runtime;
    void *object;
    std::uintptr_t module;
    std::uintptr_t monitor = 0;
    std::mutex mutex;
    bool valid = true;
    std::vector<ambrosia::services::InstallationProgress> snapshots;
};

void AMBROSIA_GUEST_ABI package_progress_body(void *context, std::uintptr_t monitor) {
    using namespace ambrosia;
    using namespace services;
    auto &record = *static_cast<PackageProgressRecord *>(context);
    const bool scoped = current_runtime() == record.runtime && current_modules() &&
                        guest::ThreadContext::current() && monitor == record.monitor &&
                        *graph_tls(reinterpret_cast<void *>(record.module)) == 41 && get_last_error() == 0;
    InstallationProgress observed;
    if (scoped) {
        const auto *table = *static_cast<const std::uintptr_t *const *>(record.object);
        using Progress = void(AMBROSIA_GUEST_ABI *)(void *, std::uintptr_t, void *);
        reinterpret_cast<Progress>(table[7])(record.object, monitor, &observed);
        *graph_tls(reinterpret_cast<void *>(record.module)) = 77;
        set_last_error(17);
    }
    const std::lock_guard lock(record.mutex);
    record.valid = record.valid && scoped;
    record.snapshots.push_back(observed);
}

void package_progress_abi_checks() {
    using namespace ambrosia;
    using namespace services;
    PackageMonitorFixture fixture("package-progress-abi");
    auto bytes = tls_fixture();
    put<std::uint32_t>(bytes, 0x98 + 120, 0);
    put<std::uint32_t>(bytes, 0x98 + 124, 0);
    jump_to_host(bytes, 0x300, reinterpret_cast<std::uintptr_t>(&worker_module_callback));
    jump_to_host(bytes, 0x380, reinterpret_cast<std::uintptr_t>(&package_progress_body));
    pe::ModuleGraph graph;
    graph.add("progress.exe", bytes);
    graph.load("progress.exe");
    Scope scope(fixture.runtime, &graph);
    auto process = graph.start();
    auto &queues = fixture.runtime.task_queues();
    auto &packages = fixture.runtime.gdk_packages();
    std::uintptr_t queue = 0;
    (void)queues.create(0, 1, &queue);
    const auto completion = queues.thread_pool_scheduler(queue, QueuePort::completion);
    check(!queues.thread_pool_scheduler(queue) && completion &&
              !queues.thread_pool_scheduler(queue, static_cast<QueuePort>(99)),
          "native completion dispatch follows its own port mode instead of borrowing manual work mode");
    const auto *table = *static_cast<const std::uintptr_t *const *>(fixture.object);
    using Create = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, const char *, std::uint32_t, const void *,
                                                      std::uint32_t, std::uintptr_t, void *);
    using Routine = void(AMBROSIA_GUEST_ABI *)(void *, std::uintptr_t);
    using Register = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, std::uintptr_t, void *, Routine, void *);
    using Unregister = bool(AMBROSIA_GUEST_ABI *)(void *, std::uintptr_t, std::uintptr_t, bool);
    using Update = bool(AMBROSIA_GUEST_ABI *)(void *, std::uintptr_t);
    const auto create = reinterpret_cast<Create>(table[5]);
    const auto add = reinterpret_cast<Register>(table[9]);
    const auto remove = reinterpret_cast<Unregister>(table[10]);
    const auto update = reinterpret_cast<Update>(table[8]);
    const auto callback = reinterpret_cast<Routine>(graph.loaded_module() + 0x1180);
    PackageProgressRecord record{&fixture.runtime, fixture.object, graph.loaded_module(), 0, {}, true, {}};
    std::array<std::byte, 16> storage;
    storage.fill(std::byte{0xcc});
    std::uintptr_t token = 0;
    std::int32_t registered = -1;
    const auto saved_errno = errno;
    process.with_guest_context([&] {
        (void)create(fixture.object, "Ambrosia.Monitor", 0, nullptr, 0, queue, &record.monitor);
        queues.close(queue);
        errno = EDOM;
        set_last_error(91);
        crt_error() = 61;
        registered = add(fixture.object, record.monitor, &record, callback, storage.data() + 3);
        std::memcpy(&token, storage.data() + 3, sizeof(token));
        check(registered == 0 && token && token != record.monitor && storage[2] == std::byte{0xcc} &&
                  storage[11] == std::byte{0xcc} && errno == EDOM && get_last_error() == 91 &&
                  crt_error() == 61,
              "console progress registration publishes a guarded opaque token and preserves all error "
              "channels");
    });
    drain_progress_queue(queues, completion);
    check(record.snapshots.empty(), "registration alone does not invent an initial progress-change event");
    PackageMonitorFixture::write(fixture.root / "assets/payload.bin", "a");
    process.with_guest_context([&] { (void)update(fixture.object, record.monitor); });
    drain_progress_queue(queues, completion);
    check(
        record.valid && record.snapshots.size() == 1 && !record.snapshots[0].completed &&
            record.snapshots[0].total_bytes == 5 && !record.snapshots[0].installed_bytes,
        "a real size change dispatches the original PE callback with correct context, handle and guest TLS");
    PackageMonitorFixture::write(fixture.root / "assets/payload.bin", "abcde");
    process.with_guest_context([&] { (void)update(fixture.object, record.monitor); });
    drain_progress_queue(queues, completion);
    check(record.valid && record.snapshots.size() == 2 && record.snapshots[1].completed,
          "restored readiness delivers another scoped callback with fresh TLS and a real complete snapshot");
    bool unchanged = false;
    process.with_guest_context([&] { unchanged = !update(fixture.object, record.monitor); });
    drain_progress_queue(queues, completion);
    check(unchanged && record.snapshots.size() == 2,
          "unchanged progress does not generate duplicate callbacks");
    process.with_guest_context([&] {
        check(remove(fixture.object, record.monitor, token, true) &&
                  remove(fixture.object, record.monitor, token, false),
              "console unregistration completes and remains idempotent after an idle token is removed");
        std::uintptr_t rejected = 99;
        check(add(fixture.object, record.monitor, &record, nullptr, &rejected) == gdk_invalid_argument &&
                  !rejected &&
                  add(fixture.object, 0x123456, &record, callback, &rejected) ==
                      static_cast<std::int32_t>(0x80070006u) &&
                  !rejected,
              "null callbacks and unknown monitor handles clear token output without registering work");
    });
    std::uintptr_t second = 0;
    (void)packages.create_monitor("Ambrosia.Monitor", 0, 0, {}, &second, completion);
    std::vector<std::uintptr_t> tokens;
    bool bounded = true;
    for (unsigned index = 0; index < GdkPackages::registration_limit; ++index) {
        std::uintptr_t owned = 0;
        const auto result = packages.register_progress(record.monitor, [] {}, &owned);
        bounded = bounded && result == 0 && owned > token;
        tokens.push_back(owned);
    }
    std::uintptr_t exhausted = 99;
    check(bounded &&
              packages.register_progress(
                  second, [] {}, &exhausted) == gdk_out_of_memory &&
              !exhausted,
          "a global registration budget spans monitors and never reuses closed token IDs");
    for (const auto owned : tokens) {
        (void)packages.unregister_progress(record.monitor, owned, true);
    }
    check(packages.register_progress(
              second, [] {}, &exhausted) == 0 &&
              exhausted > tokens.back(),
          "unregistering idle callbacks recovers global registration capacity");
    packages.close_monitor(second);
    packages.close_monitor(record.monitor);
    fixture.runtime.shutdown_workers();
    process.close();
    errno = saved_errno;
}

void package_progress_lifetime_checks() {
    using namespace ambrosia;
    using namespace services;
    using namespace std::chrono_literals;
    PackageMonitorFixture fixture("package-progress-lifetimes");
    auto &queues = fixture.runtime.task_queues();
    auto &packages = fixture.runtime.gdk_packages();
    std::uintptr_t queue = 0;
    (void)queues.create(1, 1, &queue);
    const auto work = queues.thread_pool_scheduler(queue);
    const auto completion = queues.thread_pool_scheduler(queue, QueuePort::completion);
    std::uintptr_t monitor = 0;
    (void)packages.create_monitor("Ambrosia.Monitor", 0, 0, {}, &monitor, completion);
    std::mutex mutex;
    std::condition_variable changed;
    unsigned calls = 0;
    bool released = false;
    const auto blocking = [&] {
        std::unique_lock lock(mutex);
        ++calls;
        changed.notify_all();
        if (calls == 1) {
            changed.wait(lock, [&] { return released; });
        }
    };
    const auto wait_for_entry = [&] {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 2s, [&] { return calls != 0; });
    };
    const auto wait_for_followup = [&] {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 2s, [&] { return calls >= 2; });
    };
    const auto release = [&] {
        const std::lock_guard lock(mutex);
        released = true;
        changed.notify_all();
    };
    const auto reset = [&] {
        const std::lock_guard lock(mutex);
        calls = 0;
        released = false;
    };
    const auto change = [&](std::string_view text) {
        PackageMonitorFixture::write(fixture.root / "assets/payload.bin", text);
        return packages.update_monitor(monitor);
    };
    std::uintptr_t token = 0;
    (void)packages.register_progress(monitor, blocking, &token);
    (void)change("a");
    const auto first_entered = wait_for_entry();
    (void)change("abcde");
    (void)change("ab");
    (void)change("abcde");
    release();
    const auto followed_up = wait_for_followup();
    drain_progress_queue(queues, completion);
    check(first_entered && followed_up && calls == 2,
          "changed revisions coalesce into one follow-up while a handler is active");
    (void)packages.unregister_progress(monitor, token, true);
    reset();
    (void)packages.register_progress(monitor, blocking, &token);
    (void)change("a");
    const auto pending_entered = wait_for_entry();
    const auto pending_first = packages.unregister_progress(monitor, token, false);
    const auto pending_second = packages.unregister_progress(monitor, token, false);
    (void)change("abcde");
    release();
    drain_progress_queue(queues, completion);
    check(pending_entered && !pending_first && !pending_second && calls == 1 &&
              packages.unregister_progress(monitor, token, false),
          "nonwaiting unregister stays pending until the active handler finishes and cancels later changes");
    reset();
    (void)packages.register_progress(monitor, blocking, &token);
    (void)change("a");
    const auto waiting_entered = wait_for_entry();
    std::atomic<bool> removed = false;
    std::thread waiter([&] { removed = packages.unregister_progress(monitor, token, true); });
    bool premature = false;
    {
        std::unique_lock lock(mutex);
        premature = changed.wait_for(lock, 20ms, [&] { return removed.load(); });
    }
    release();
    waiter.join();
    drain_progress_queue(queues, completion);
    check(waiting_entered && !premature && removed && calls == 1,
          "waiting unregister joins its in-flight handler before permitting context release");
    packages.close_monitor(monitor);

    (void)packages.create_monitor("Ambrosia.Monitor", 0, 5, work, &monitor, completion);
    reset();
    std::atomic<bool> read_during_close = false;
    (void)packages.register_progress(
        monitor,
        [&] {
            blocking();
            try {
                (void)packages.progress(monitor);
                read_during_close = true;
            } catch (const std::runtime_error &) {
            }
        },
        &token);
    (void)change("abcde");
    const auto closing_entered = wait_for_entry();
    std::atomic<bool> closed = false;
    std::thread closer([&] {
        packages.close_monitor(monitor);
        closed = true;
    });
    bool closing_seen = false;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        std::uintptr_t extra = 0;
        const auto result = packages.register_progress(monitor, [] {}, &extra);
        if (result == static_cast<std::int32_t>(0x80070032u)) {
            closing_seen = true;
            break;
        }
        if (result != 0) {
            break;
        }
        (void)packages.unregister_progress(monitor, extra, true);
        std::this_thread::sleep_for(1ms);
    }
    const bool close_returned_early = closed;
    release();
    closer.join();
    drain_progress_queue(queues, completion);
    check(closing_entered && closing_seen && !close_returned_early && closed && read_during_close,
          "monitor close waits outside directory locks and retains cached progress for its active handler");
    (void)packages.create_monitor("Ambrosia.Monitor", 0, 0, {}, &monitor, completion);
    std::atomic<bool> self_closed = false;
    (void)packages.register_progress(
        monitor,
        [&] {
            packages.close_monitor(monitor);
            self_closed = true;
        },
        &token);
    (void)change("a");
    drain_progress_queue(queues, completion);
    check(self_closed && runtime_rejected([&] { (void)packages.progress(monitor); }),
          "a handler can close its own monitor without waiting for itself or leaving a live handle");
    fixture.runtime.shutdown_workers();
}
