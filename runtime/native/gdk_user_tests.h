unsigned user_change_calls = 0;

void AMBROSIA_GUEST_ABI user_change_callback(void *, std::uint64_t, std::uint32_t) {
    ++user_change_calls;
}

void gdk_user_subscription_checks() {
    using namespace ambrosia::services;
    Runtime runtime;
    Runtime independent;
    Scope scope(runtime);
    (void)runtime.gdk_bootstrap().initialize(1, 2);
    using Query = std::int32_t(AMBROSIA_GUEST_ABI *)(const void *, const void *, void *);
    const auto query =
        reinterpret_cast<Query>(runtime.resolve(service_symbol("QueryApiImpl", "xgameruntime.dll")));
    void *object = nullptr;
    const auto result = query(&user_class_id, &user_class_id, &object);
    check(result == 0 && object && runtime.gdk_users().references() == 2 &&
              independent.gdk_users().references() == 1,
          "base XUser class owns a counted interface per runtime without allocating or signing in a user");
    const auto *table = *static_cast<const std::uintptr_t *const *>(object);
    using InterfaceQuery = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, const void *, void *);
    using Release = std::uint32_t(AMBROSIA_GUEST_ABI *)(void *);
    using Maximum = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, void *);
    using Register =
        std::int32_t(AMBROSIA_GUEST_ABI *)(void *, std::uintptr_t, void *, UserChangeRoutine, void *);
    using Unregister = bool(AMBROSIA_GUEST_ABI *)(void *, std::uint64_t, bool);
    const auto qi = reinterpret_cast<InterfaceQuery>(table[0]);
    const auto release = reinterpret_cast<Release>(table[2]);
    const auto maximum = reinterpret_cast<Maximum>(table[6]);
    const auto subscribe = reinterpret_cast<Register>(table[33]);
    const auto unsubscribe = reinterpret_cast<Unregister>(table[34]);
    void *unknown = nullptr;
    const auto queried = qi(object, &unknown_interface_id, &unknown);
    check(queried == 0 && unknown == object && release(unknown) == 2,
          "XUser IUnknown retains the same counted identity and releases to the runtime pin plus caller");
    const Guid absent{};
    unknown = reinterpret_cast<void *>(1);
    check(qi(object, &absent, &unknown) == no_interface && !unknown,
          "unsupported user interface revisions cannot obtain an invented extension vtable");
    std::array<std::byte, 12> guarded;
    guarded.fill(std::byte{0xcc});
    const auto max_status = maximum(object, guarded.data() + 3);
    std::uint32_t capacity = 0;
    std::memcpy(&capacity, guarded.data() + 3, sizeof(capacity));
    check(max_status == 0 && capacity == 1 && guarded[2] == std::byte{0xcc} &&
              guarded[7] == std::byte{0xcc} && maximum(object, nullptr) == pointer_error,
          "maximum users is the explicit one-user provider capacity, written through exactly four unaligned "
          "bytes");
    std::uintptr_t queue = 0;
    const auto created = runtime.task_queues().create(0, 0, &queue);
    std::weak_ptr<const QueueState> retained = runtime.task_queues().inspect(queue);
    user_change_calls = 0;
    int context = 7;
    const auto saved_error = errno;
    errno = EDOM;
    crt_error() = 61;
    set_last_error(91);
    guarded.fill(std::byte{0xcc});
    const auto registered = subscribe(object, queue, &context, &user_change_callback, guarded.data() + 1);
    std::uint64_t token = 0;
    std::memcpy(&token, guarded.data() + 1, sizeof(token));
    auto snapshot = runtime.gdk_users().subscription(token);
    check(created == 0 && registered == 0 && token && token != queue && snapshot &&
              snapshot->context == &context && snapshot->routine == &user_change_callback &&
              snapshot->queue == retained.lock() && guarded[0] == std::byte{0xcc} &&
              guarded[9] == std::byte{0xcc} && runtime.gdk_users().subscriptions() == 1 &&
              !independent.gdk_users().subscriptions() && user_change_calls == 0 && errno == EDOM &&
              crt_error() == 61 && get_last_error() == 91,
          "user registration retains actual callback/context/queue under a distinct eight-byte token, "
          "without an invented initial event or error changes");
    snapshot.reset();
    runtime.task_queues().close(queue);
    check(!retained.expired() && runtime.gdk_users().subscription(token).has_value(),
          "closing the caller queue handle leaves its actual queue state retained by the user subscription");
    const auto removed = unsubscribe(object, token, false);
    const auto repeated = unsubscribe(object, token, true);
    check(removed && repeated && retained.expired() && !runtime.gdk_users().subscriptions() &&
              !user_change_calls,
          "nonwaiting and repeated waiting removal finish immediately without dispatching callbacks when no "
          "user event is active");
    std::uint64_t invalid_token = 99;
    const auto missing_callback = subscribe(object, queue, nullptr, nullptr, &invalid_token);
    const auto no_token = subscribe(object, queue, nullptr, &user_change_callback, nullptr);
    const auto missing_queue = subscribe(object, queue, nullptr, &user_change_callback, &invalid_token);
    check(missing_callback == pointer_error && no_token == pointer_error &&
              missing_queue == static_cast<std::int32_t>(0x80070006u) && !invalid_token &&
              !runtime.gdk_users().subscriptions(),
          "null callbacks/output and closed queues cannot publish subscription tokens or success");
    (void)runtime.task_queues().create(0, 0, &queue);
    using FlatRegister =
        std::int32_t(AMBROSIA_GUEST_ABI *)(std::uintptr_t, void *, UserChangeRoutine, void *);
    const auto flat_register = reinterpret_cast<FlatRegister>(
        runtime.resolve(service_symbol("XUserRegisterForChangeEvent", "xgameruntime.dll")));
    std::uint64_t flat_token = 0;
    const auto flat_status = flat_register(queue, &context, &user_change_callback, &flat_token);
    check(flat_status == 0 && flat_token > token && runtime.gdk_users().subscription(flat_token).has_value(),
          "flat and interface registration share one owned registry and never reuse a closed token");
    (void)unsubscribe(object, flat_token, false);
    std::vector<std::uint64_t> tokens;
    bool bounded = true;
    for (unsigned index = 0; index < GdkUsers::registration_limit; ++index) {
        std::uint64_t next = 0;
        const auto status = subscribe(object, queue, &context, &user_change_callback, &next);
        bounded = bounded && status == 0 && next && (tokens.empty() || next > tokens.back());
        tokens.push_back(next);
    }
    invalid_token = 99;
    const auto exhausted = subscribe(object, queue, &context, &user_change_callback, &invalid_token);
    check(
        bounded && exhausted == gdk_out_of_memory && !invalid_token &&
            runtime.gdk_users().subscriptions() == GdkUsers::registration_limit,
        "user subscriptions enforce the bounded live-entry budget and never publish an output on exhaustion");
    bool released = true;
    for (const auto value : tokens) {
        released = unsubscribe(object, value, false) && released;
    }
    runtime.task_queues().close(queue);
    check(released && !runtime.gdk_users().subscriptions() && !user_change_calls && release(object) == 1 &&
              runtime.gdk_users().references() == 1,
          "all subscriptions and queried interface references return to their owned baseline without "
          "invoking guest code");
    errno = saved_error;
}
