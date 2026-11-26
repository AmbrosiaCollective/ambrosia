void delayed_queue_checks() {
    using namespace ambrosia;
    using namespace std::chrono_literals;
    services::Runtime runtime;
    services::Scope scope(runtime);
    guest::ThreadContext context;
    auto activation = context.activate();
    auto &queues = runtime.task_queues();
    std::uintptr_t handle = 0;
    check(queues.create(0, 1, &handle) == 0 && !queues.thread_pool_scheduler(handle) &&
              !queues.thread_pool_scheduler(0),
          "native timers refuse unknown handles and unsupported manual work mode");
    queues.close(handle);
    check(queues.create(1, 1, &handle) == 0, "native timers use the observed real thread-pool queue mode");
    auto scheduler = queues.thread_pool_scheduler(handle);
    const auto queue = queues.inspect(handle);
    queues.close(handle);
    check(scheduler && !queues.inspect(handle) && queue,
          "a native scheduler retains its queue after the public handle closes");

    std::mutex mutex;
    std::condition_variable changed;
    bool finished = false;
    bool isolated = false;
    std::chrono::steady_clock::duration elapsed{};
    const auto start = std::chrono::steady_clock::now();
    const auto owner_thread = std::this_thread::get_id();
    auto owner = std::make_shared<int>(1);
    const auto accepted = scheduler(25, owner, [&] {
        std::lock_guard lock(mutex);
        elapsed = std::chrono::steady_clock::now() - start;
        isolated = std::this_thread::get_id() != owner_thread && !services::current_runtime() &&
                   !guest::ThreadContext::current() && host::guest_segment_base() == 0;
        finished = true;
        changed.notify_all();
    });
    bool completed = false;
    {
        std::unique_lock lock(mutex);
        completed = changed.wait_for(lock, 2s, [&] { return finished; });
    }
    check(accepted && completed && elapsed >= 25ms && isolated,
          "a monotonic native timer runs no earlier than due on a real worker with cleared guest context");

    host::WorkerPool pool;
    check(!pool.submit_after(0ms, owner, [] {}), "unstarted pools cannot accept delayed adapters");
    pool.start();
    check(!pool.submit_after(-1ms, owner, [] {}) &&
              !pool.submit_after(std::chrono::milliseconds(1ull << 32), owner, [] {}) &&
              !pool.submit_after(0ms, {}, [] {}) && !pool.submit_after(0ms, owner, {}),
          "invalid delays, expired owners and missing callbacks cannot enter the timer queue");
    auto cancelled_owner = std::make_shared<int>(2);
    std::atomic<unsigned> cancelled_calls = 0;
    bool filled = true;
    for (unsigned index = 0; index < host::WorkerPool::pending_limit; ++index) {
        filled = filled && pool.submit_after(24h, cancelled_owner, [&] { ++cancelled_calls; });
    }
    check(filled && !pool.submit_after(24h, owner, [] {}) && !pool.submit([] {}),
          "delayed and immediate jobs share one bounded pending-work budget");
    cancelled_owner.reset();
    finished = false;
    const auto recovered = pool.submit_after(0ms, owner, [&] {
        std::lock_guard lock(mutex);
        finished = true;
        changed.notify_all();
    });
    {
        std::unique_lock lock(mutex);
        completed = changed.wait_for(lock, 2s, [&] { return finished; });
    }
    check(recovered && completed && cancelled_calls.load() == 0,
          "expired timer owners recover pending capacity and earlier deadlines wake waiting workers");
    auto pending_owner = std::make_shared<int>(3);
    const auto pending = pool.submit_after(24h, pending_owner, [&] { ++cancelled_calls; });
    const auto before_close = std::chrono::steady_clock::now();
    pool.shutdown();
    check(pending && std::chrono::steady_clock::now() - before_close < 2s && cancelled_calls.load() == 0 &&
              !pool.submit_after(0ms, owner, [] {}),
          "shutdown cancels future timers without waiting for deadlines or accepting new work");
}
