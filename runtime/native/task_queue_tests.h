void task_queue_manual_checks() {
    using namespace ambrosia::services;
    Runtime runtime;
    Scope scope(runtime);
    auto &queues = runtime.task_queues();
    std::uintptr_t handle = 99;
    check(task_queue_create(4, 0, &handle) == gdk_invalid_argument && !handle &&
              task_queue_create(0, 99, &handle) == gdk_invalid_argument && !handle &&
              task_queue_create(0, 0, nullptr) == gdk_invalid_argument && !queues.workers(),
          "invalid queue modes/output are rejected before starting any native worker");
    const auto saved_error = errno;
    errno = EDOM;
    crt_error() = 91;
    set_last_error(27);
    check(task_queue_create(0, 3, &handle) == 0 && handle && queues.workers() == 0,
          "manual/immediate queue creation owns a handle without starting a worker");
    const auto state = queues.inspect(handle);
    check(state && state->work == QueueMode::manual && state->completion == QueueMode::immediate,
          "queue state retains independent work and completion modes");
    std::uintptr_t duplicate = 0;
    const auto duplicated = task_queue_duplicate(handle, &duplicate);
    check(duplicated == 0 && duplicate && duplicate != handle && queues.inspect(duplicate) == state,
          "queue duplication creates a distinct handle to the same owned queue");
    task_queue_close(handle);
    check(!queues.inspect(handle) && queues.inspect(duplicate) == state,
          "closing one queue handle preserves the duplicate's object lifetime");
    std::uintptr_t missing = 9;
    const auto invalid = task_queue_duplicate(handle, &missing);
    check(invalid == static_cast<std::int32_t>(0x80070006u) && !missing,
          "stale queue handles return E_HANDLE and clear output");
    task_queue_close(handle);
    task_queue_close(0);
    task_queue_close(duplicate);
    check(!queues.inspect(duplicate) && errno == EDOM && crt_error() == 91 && get_last_error() == 27,
          "closing stale/null handles is harmless and queue services preserve error channels");
    std::array<std::byte, 16> guarded;
    guarded.fill(std::byte{0xcc});
    const auto unaligned = task_queue_create(3, 0, guarded.data() + 3);
    std::memcpy(&handle, guarded.data() + 3, sizeof(handle));
    check(unaligned == 0 && handle && guarded[2] == std::byte{0xcc} && guarded[11] == std::byte{0xcc},
          "queue output copying supports unaligned guest pointers and preserves guards");
    task_queue_close(handle);
    std::vector<std::uintptr_t> handles;
    bool created = true;
    for (unsigned index = 0; index < TaskQueues::handle_limit; ++index) {
        const auto result = task_queue_create(0, 0, &handle);
        created = created && result == 0 && handle;
        handles.push_back(handle);
    }
    handle = 9;
    const auto exhausted = task_queue_create(0, 0, &handle);
    const auto duplicate_exhausted = task_queue_duplicate(handles.front(), &missing);
    check(created && exhausted == gdk_out_of_memory && !handle && duplicate_exhausted == gdk_out_of_memory &&
              !missing,
          "creation and duplication enforce the per-runtime queue handle budget transactionally");
    for (const auto value : handles) {
        task_queue_close(value);
    }
    const auto recovered = task_queue_create(0, 0, &handle);
    check(recovered == 0 && handle > handles.back(),
          "closed handles recover capacity without reusing stale queue IDs");
    task_queue_close(handle);
    Runtime other;
    check(!other.task_queues().inspect(handles.front()), "queue directories are owned by each runtime");
    errno = saved_error;
}

void worker_pool_checks() {
    using namespace ambrosia;
    using namespace std::chrono_literals;
    services::Runtime runtime;
    services::Scope runtime_scope(runtime);
    guest::ThreadContext context;
    auto activation = context.activate();
    host::WorkerPool pool;
    check(!pool.submit([] {}) && pool.workers() == 0, "an unstarted worker pool does not accept jobs");
    pool.start();
    pool.start();
    check(pool.workers() == 2, "native worker startup is bounded and idempotent");
    std::mutex mutex;
    std::condition_variable ready;
    unsigned started = 0;
    unsigned finished = 0;
    bool released = false;
    bool isolated_contexts = true;
    std::set<std::thread::id> worker_ids;
    const auto owner = std::this_thread::get_id();
    const auto blocker = [&] {
        std::unique_lock lock(mutex);
        worker_ids.insert(std::this_thread::get_id());
        isolated_contexts = isolated_contexts && !services::current_runtime() &&
                            !guest::ThreadContext::current() && host::guest_segment_base() == 0;
        ++started;
        ready.notify_all();
        ready.wait(lock, [&] { return released; });
        ++finished;
    };
    const auto first = pool.submit(blocker);
    const auto second = pool.submit(blocker);
    bool concurrent = false;
    {
        std::unique_lock lock(mutex);
        concurrent = ready.wait_for(lock, 2s, [&] { return started == 2; });
    }
    unsigned drained = 0;
    bool accepted = true;
    for (unsigned index = 0; index < host::WorkerPool::pending_limit; ++index) {
        const auto submitted = pool.submit([&] {
            std::lock_guard lock(mutex);
            ++drained;
        });
        accepted = accepted && submitted;
    }
    const auto overflow = pool.submit([] {});
    {
        std::lock_guard lock(mutex);
        released = true;
    }
    ready.notify_all();
    pool.shutdown();
    check(first && second && concurrent && worker_ids.size() == 2 && !worker_ids.contains(owner),
          "two real worker threads execute concurrently instead of running on the submitting thread");
    check(isolated_contexts,
          "native workers clear inherited GS and do not inherit the submitting guest/runtime context");
    check(accepted && !overflow && drained == host::WorkerPool::pending_limit && finished == 2,
          "the scheduler bounds pending jobs and drains accepted work before joining");
    check(pool.workers() == 0 && !pool.submit([] {}), "closed pools reject work after all workers join");
    pool.shutdown();
    check(runtime_rejected([&] { pool.start(); }), "closed worker pools cannot be accidentally restarted");
}

void task_queue_thread_checks() {
    using namespace ambrosia::services;
    Runtime runtime;
    Scope scope(runtime);
    void *object = nullptr;
    (void)runtime.gdk_threading().query(threading_class_id, &object);
    const auto *table = *static_cast<const std::uintptr_t *const *>(object);
    using Create = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, std::uint32_t, std::uint32_t, void *);
    using Duplicate = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, std::uintptr_t, void *);
    using Close = void(AMBROSIA_GUEST_ABI *)(void *, std::uintptr_t);
    const auto create = reinterpret_cast<Create>(table[12]);
    const auto duplicate = reinterpret_cast<Duplicate>(table[15]);
    const auto close = reinterpret_cast<Close>(table[17]);
    bool modes_match = true;
    for (std::uint32_t work = 0; work < 4; ++work) {
        for (std::uint32_t completion = 0; completion < 4; ++completion) {
            std::uintptr_t handle = 0;
            const auto result = create(object, work, completion, &handle);
            const auto state = runtime.task_queues().inspect(handle);
            modes_match = modes_match && result == 0 && state &&
                          state->work == static_cast<QueueMode>(work) &&
                          state->completion == static_cast<QueueMode>(completion);
            std::uintptr_t copy = 0;
            const auto duplicated = duplicate(object, handle, &copy);
            close(object, handle);
            modes_match = modes_match && duplicated == 0 && copy != handle &&
                          runtime.task_queues().inspect(copy) == state;
            close(object, copy);
        }
    }
    check(modes_match,
          "all dispatch-mode pairs retain their state and lifetime through real Windows vtable calls");
    check(runtime.task_queues().workers() == 2, "all queues share a bounded real native scheduler");
    (void)interface_release(object);
}
