struct AppNotificationRecord {
    ambrosia::services::Runtime *runtime = nullptr;
    std::vector<unsigned> values;
    bool guest_context = true;
    std::uintptr_t self = 0;
    std::uintptr_t remove_next = 0;
    std::uintptr_t added = 0;
    AppNotificationRecord *new_record = nullptr;
};

void AMBROSIA_GUEST_ABI app_changed(std::uint8_t value, void *opaque) {
    using namespace ambrosia;
    using namespace services;
    auto &record = *static_cast<AppNotificationRecord *>(opaque);
    record.values.push_back(value);
    record.guest_context = record.guest_context && current_runtime() == record.runtime &&
                           guest::ThreadContext::current() && host::guest_segment_base();
    if (record.self) {
        record.runtime->app_lifecycle().remove(AppNotification::state, record.self);
        record.self = 0;
    }
    if (record.remove_next) {
        record.runtime->app_lifecycle().remove(AppNotification::state, record.remove_next);
        record.remove_next = 0;
    }
    if (record.new_record && !record.added) {
        (void)record.runtime->app_lifecycle().add(AppNotification::state, &app_changed, record.new_record,
                                                  &record.added);
    }
}

void app_lifecycle_checks() {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    Runtime independent;
    Scope scope(runtime);
    using Register = std::uint32_t(AMBROSIA_GUEST_ABI *)(AppChangeRoutine, void *, void *);
    using Unregister = void(AMBROSIA_GUEST_ABI *)(std::uintptr_t);
    const auto reg = reinterpret_cast<Register>(runtime.resolve(
        service_symbol("RegisterAppStateChangeNotification", "api-ms-win-core-psm-appnotify-l1-1-0.dll")));
    const auto unreg = reinterpret_cast<Unregister>(runtime.resolve(
        service_symbol("UnregisterAppStateChangeNotification", "api-ms-win-core-psm-appnotify-l1-1-0.dll")));
    const auto constrained = reinterpret_cast<Register>(runtime.resolve(service_symbol(
        "RegisterAppConstrainedChangeNotification", "api-ms-win-core-psm-appnotify-l1-1-1.dll")));
    const auto unconstrained = reinterpret_cast<Unregister>(
        runtime.resolve(service_symbol("UnregisterAppConstrainedChangeNotification", "xgameruntime.dll")));
    check(reg && unreg && constrained && unconstrained,
          "state and constrained registration services resolve through owned native aliases");
    AppNotificationRecord state{&runtime, {}};
    AppNotificationRecord constraint{&runtime, {}};
    std::uintptr_t state_handle = 9;
    check(reg(nullptr, &state, &state_handle) == 87 && !state_handle &&
              reg(&app_changed, &state, nullptr) == 87,
          "invalid notification registration leaves no callback or output handle");
    const auto saved_error = errno;
    errno = EDOM;
    crt_error() = 61;
    set_last_error(37);
    std::uintptr_t constrained_handle = 0;
    const auto state_status = reg(&app_changed, &state, &state_handle);
    const auto constrained_status = constrained(&app_changed, &constraint, &constrained_handle);
    check(
        state_status == 0 && constrained_status == 0 && state_handle != constrained_handle &&
            runtime.app_lifecycle().count(AppNotification::state) == 1 &&
            runtime.app_lifecycle().count(AppNotification::constrained) == 1 && state.values.empty(),
        "registration owns callback/context pairs and distinct kind-specific IDs without fabricated events");
    check(independent.app_lifecycle().count(AppNotification::state) == 0,
          "notification state is runtime-owned instead of a process-global singleton");
    check(errno == EDOM && crt_error() == 61 && get_last_error() == 37,
          "registration preserves native errno and the caller's guest error channels");
    check(runtime_rejected([&] { runtime.notify_app_lifecycle(AppNotification::state, true); }),
          "notification delivery refuses a host thread without active guest TLS");
    guest::ThreadContext thread;
    auto activation = thread.activate();
    {
        Scope different(independent);
        check(runtime_rejected([&] { runtime.notify_app_lifecycle(AppNotification::state, true); }),
              "delivery refuses a different active runtime even with guest TLS present");
    }
    const auto suspended = runtime.notify_app_lifecycle(AppNotification::state, true);
    const auto resumed = runtime.notify_app_lifecycle(AppNotification::state, false);
    check(suspended == 1 && resumed == 1 && state.values == std::vector<unsigned>({1, 0}) &&
              constraint.values.empty() && state.guest_context,
          "state delivery calls the stored Windows callback with byte BOOLEAN, context and active guest TLS");
    const auto restricted = runtime.notify_app_lifecycle(AppNotification::constrained, true);
    const auto restored = runtime.notify_app_lifecycle(AppNotification::constrained, false);
    check(restricted == 1 && restored == 1 && constraint.values == std::vector<unsigned>({1, 0}),
          "constrained notifications have their own subscribers and preserve both boolean values");
    unconstrained(state_handle);
    check(runtime.app_lifecycle().count(AppNotification::state) == 1,
          "the wrong unregister kind cannot consume another notification registration");
    unreg(state_handle);
    unreg(state_handle);
    unreg(0);
    unconstrained(constrained_handle);
    const auto after_unregister = runtime.notify_app_lifecycle(AppNotification::state, true);
    check(after_unregister == 0 && runtime.app_lifecycle().count(AppNotification::constrained) == 0,
          "unregistration removes callbacks, tolerates stale/null IDs and prevents further delivery");
    check(errno == EDOM && crt_error() == 0 && get_last_error() == 0,
          "native errno is preserved and new guest TLS starts with independent error state");
    std::array<std::byte, 16> guarded;
    guarded.fill(std::byte{0xcc});
    const auto unaligned = reg(&app_changed, &state, guarded.data() + 3);
    std::memcpy(&state_handle, guarded.data() + 3, sizeof(state_handle));
    check(unaligned == 0 && state_handle && guarded[2] == std::byte{0xcc} && guarded[11] == std::byte{0xcc},
          "registration output copying accepts unaligned guest pointers and preserves guards");
    unreg(state_handle);

    AppNotificationRecord first{&runtime, {}};
    AppNotificationRecord removed{&runtime, {}};
    AppNotificationRecord added{&runtime, {}};
    const auto registered_first = reg(&app_changed, &first, &first.self);
    const auto registered_second = reg(&app_changed, &removed, &first.remove_next);
    first.new_record = &added;
    const auto delivered = runtime.notify_app_lifecycle(AppNotification::state, true);
    check(registered_first == 0 && registered_second == 0 && delivered == 1 &&
              first.values == std::vector<unsigned>({1}) && removed.values.empty() && added.values.empty(),
          "callbacks can remove themselves and a later snapshot entry; new registrations wait for the next "
          "batch");
    const auto next_batch = runtime.notify_app_lifecycle(AppNotification::state, false);
    check(next_batch == 1 && added.values == std::vector<unsigned>({0}) && first.guest_context,
          "callback-time registration retains its context after the original callback unregisters itself");
    unreg(first.added);
    std::vector<std::uintptr_t> registrations;
    bool filled = true;
    for (unsigned index = 0; index < AppLifecycle::registration_limit; ++index) {
        std::uintptr_t handle = 0;
        const auto result = reg(&app_changed, &state, &handle);
        filled = filled && !result && handle;
        registrations.push_back(handle);
    }
    state_handle = 9;
    const auto overflow = constrained(&app_changed, &constraint, &state_handle);
    check(filled && overflow == 8 && !state_handle &&
              runtime.app_lifecycle().count(AppNotification::state) == AppLifecycle::registration_limit,
          "both subscription kinds share a bounded transactional registration budget");
    for (const auto handle : registrations) {
        unreg(handle);
    }
    const auto recovered = reg(&app_changed, &state, &state_handle);
    check(recovered == 0 && state_handle > registrations.back(),
          "unregister recovers capacity without reusing stale registration IDs");
    unreg(state_handle);
    errno = saved_error;
}

struct BlockingAppNotification {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool release = false;
    bool unregister_started = false;
    bool unregister_finished = false;
};

void AMBROSIA_GUEST_ABI blocking_app_changed(std::uint8_t, void *opaque) {
    auto &record = *static_cast<BlockingAppNotification *>(opaque);
    std::unique_lock lock(record.mutex);
    record.entered = true;
    record.changed.notify_all();
    record.changed.wait(lock, [&] { return record.release; });
}

void app_lifecycle_thread_checks() {
    using namespace ambrosia;
    using namespace services;
    using namespace std::chrono_literals;
    Runtime runtime;
    BlockingAppNotification record;
    std::uintptr_t registration = 0;
    const auto registered =
        runtime.app_lifecycle().add(AppNotification::state, &blocking_app_changed, &record, &registration);
    host::WorkerPool pool;
    pool.start();
    unsigned delivered = 0;
    const auto submitted = pool.submit([&] {
        Scope scope(runtime);
        guest::ThreadContext thread;
        auto activation = thread.activate();
        delivered = runtime.notify_app_lifecycle(AppNotification::state, true);
    });
    bool entered = false;
    {
        std::unique_lock lock(record.mutex);
        entered = record.changed.wait_for(lock, 2s, [&] { return record.entered; });
    }
    const auto removal_submitted = pool.submit([&] {
        {
            std::lock_guard lock(record.mutex);
            record.unregister_started = true;
        }
        record.changed.notify_all();
        runtime.app_lifecycle().remove(AppNotification::state, registration);
        {
            std::lock_guard lock(record.mutex);
            record.unregister_finished = true;
        }
        record.changed.notify_all();
    });
    bool removal_started = false;
    bool returned_early = false;
    {
        std::unique_lock lock(record.mutex);
        removal_started = record.changed.wait_for(lock, 2s, [&] { return record.unregister_started; });
        returned_early = record.changed.wait_for(lock, 50ms, [&] { return record.unregister_finished; });
        record.release = true;
    }
    record.changed.notify_all();
    pool.shutdown();
    check(registered == 0 && submitted && removal_submitted && entered && removal_started &&
              !returned_early && record.unregister_finished && delivered == 1 &&
              !runtime.app_lifecycle().count(AppNotification::state),
          "cross-thread unregister waits for the active guest callback before the context can be freed");
}
