void semaphore_checks() {
    using namespace ambrosia::services;
    SemaphoreSignal counted(2, 3);
    check(counted.poll() && counted.poll() && !counted.poll(),
          "semaphore polls consume one available permit without coalescing multiple initial permits");
    auto released = counted.release(2);
    check(!released.error && released.previous == 0 && counted.release(2).error == 298 && counted.poll() &&
              counted.poll() && !counted.poll(),
          "semaphore release preserves previous count and rejects maximum overflow without mutation");
    check(counted.release(0).error == 87 && counted.release(-1).error == 87 && !counted.poll(),
          "zero and negative semaphore releases cannot reduce or manufacture permits");
    SemaphoreSignal largest(0x7fffffff, 0x7fffffff);
    check(largest.release(1).error == 298 && largest.poll(),
          "semaphore maximum check avoids signed overflow at the largest legal count");
    SemaphoreSignal handoff(0, 1);
    SemaphoreSignal::Waiter first, second;
    handoff.arm(first);
    handoff.arm(second);
    check(handoff.release(2).error == 298 && !first.released && !second.released && !handoff.poll(),
          "semaphore limit is checked before waking waiters even when they could consume the released batch");
    released = handoff.release(1);
    check(!released.error && released.previous == 0 && first.released && !second.released && !handoff.poll(),
          "committed semaphore handoff cannot be stolen by an arriving poll");
    check(!handoff.release(1).error && second.released && !handoff.poll(),
          "a second semaphore release hands off a distinct permit without duplicating the first");
    SemaphoreSignal cancelled(0, 2);
    SemaphoreSignal::Waiter removed;
    cancelled.arm(removed);
    cancelled.cancel(removed);
    check(!cancelled.release(2).error && !removed.released && cancelled.poll() && cancelled.poll() &&
              !cancelled.poll(),
          "cancelled semaphore waiter leaves no stale registration or lost available permit");
    SemaphoreSignal bounded_signal(0, 1);
    std::array<SemaphoreSignal::Waiter, 65> waiters{};
    bool armed = true;
    for (unsigned index = 0; index < 64; ++index) {
        armed = armed && bounded_signal.arm(waiters[index]) == SemaphoreSignal::ArmResult::queued;
    }
    check(armed && bounded_signal.arm(waiters.back()) == SemaphoreSignal::ArmResult::exhausted &&
              !bounded_signal.release(1).error && waiters.front().released && !waiters.back().released,
          "semaphore waiter budget bounds registrations without treating maximum count as waiter capacity");
    for (auto &waiter : waiters) {
        bounded_signal.cancel(waiter);
    }

    KernelObjects objects;
    check(objects.create_semaphore(-1, 1).error == 87 && objects.create_semaphore(0, 0).error == 87 &&
              objects.create_semaphore(2, 1).error == 87,
          "invalid semaphore initial/maximum ranges fail before allocating an object handle");
    const auto semaphore = objects.create_semaphore(0, 3, u"shared-semaphore");
    const auto alias = objects.create_semaphore(1, 1, u"shared-semaphore");
    check(semaphore.handle && alias.handle && semaphore.handle != alias.handle && alias.error == 183 &&
              objects.wait(alias.handle, 0).result == wait_timeout,
          "named semaphore aliases share original initial state and independently owned handles");
    check(!objects.release_semaphore(alias.handle, 3).error &&
              objects.wait(semaphore.handle, 0).result == wait_object_0 &&
              objects.wait(alias.handle, 0).result == wait_object_0 &&
              objects.wait(alias.handle, 0).result == wait_object_0 &&
              objects.wait(alias.handle, 1).result == wait_timeout,
          "existing semaphore retains its original maximum and actual count across aliases and finite waits");
    check(objects.create_event(false, false, u"shared-semaphore").error == 6 &&
              objects.set_event(alias.handle) == 6 && objects.reset_event(alias.handle) == 6,
          "semaphore names and handles cannot be mistaken for event ownership");
    const auto event = objects.create_event(false, false, u"event-collision");
    check(event.handle && objects.create_semaphore(0, 1, u"event-collision").error == 6 &&
              objects.release_semaphore(event.handle, 1).error == 6,
          "event/semaphore namespace collision is symmetric and does not change the original object");
    objects.close(semaphore.handle);
    check(!objects.release_semaphore(alias.handle, 1).error &&
              objects.wait(alias.handle, 0).result == wait_object_0 && objects.close(alias.handle) == 0 &&
              objects.release_semaphore(alias.handle, 1).error == 6,
          "closing one semaphore alias retains state until the last handle closes");
    const auto recreated = objects.create_semaphore(1, 1, u"shared-semaphore");
    check(recreated.handle && !recreated.error && objects.wait(recreated.handle, 0).result == wait_object_0 &&
              objects.release_semaphore(recreated.handle, 2).error == 298,
          "last-close releases named semaphore identity and recreation uses the new maximum");
    const auto modifier = objects.create_semaphore(0, 1, u"rights", object_modify_state);
    const auto observer = objects.create_semaphore(0, 1, u"rights", object_synchronize);
    check(modifier.handle && observer.handle && objects.wait(modifier.handle, 0).error == 5 &&
              objects.release_semaphore(observer.handle, 1).error == 5 &&
              !objects.release_semaphore(modifier.handle, 1).error &&
              objects.wait(observer.handle, 0).result == wait_object_0,
          "semaphore synchronization and modification permissions belong to each handle independently");
    KernelObjects limited(1);
    const auto one = limited.create_event(false, false);
    check(one.handle && limited.create_semaphore(0, 1).error == 8 && limited.close(one.handle) == 0 &&
              limited.create_semaphore(0, 1).handle != one.handle,
          "events and semaphores share the bounded opaque directory without resurrecting stale IDs");

    Runtime runtime;
    Scope scope(runtime);
    using CreateW = void *(AMBROSIA_GUEST_ABI *)(void *, std::int32_t, std::int32_t, const char16_t *,
                                                 std::uint32_t, std::uint32_t);
    using CreateA = void *(AMBROSIA_GUEST_ABI *)(void *, std::int32_t, std::int32_t, const char *,
                                                 std::uint32_t, std::uint32_t);
    using Release = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, std::int32_t, std::int32_t *);
    using Wait = std::uint32_t(AMBROSIA_GUEST_ABI *)(void *, std::uint32_t);
    using Close = std::int32_t(AMBROSIA_GUEST_ABI *)(void *);
    const auto create = reinterpret_cast<CreateW>(
        runtime.resolve(service_symbol("CreateSemaphoreExW", "api-ms-win-core-synch-l1-1-0.dll")));
    const auto create_a =
        reinterpret_cast<CreateA>(runtime.resolve(service_symbol("CreateSemaphoreExA", "kernel32.dll")));
    const auto release =
        reinterpret_cast<Release>(runtime.resolve(service_symbol("ReleaseSemaphore", "kernel32.dll")));
    const auto wait =
        reinterpret_cast<Wait>(runtime.resolve(service_symbol("WaitForSingleObject", "kernel32.dll")));
    const auto close =
        reinterpret_cast<Close>(runtime.resolve(service_symbol("CloseHandle", "kernel32.dll")));
    set_last_error(71);
    crt_error() = 43;
    auto *handle = create(nullptr, 0, 2, nullptr, 0, object_all_access);
    check(
        handle && get_last_error() == 0 && crt_error() == 43,
        "six-argument semaphore Microsoft ABI receives flags/access correctly and records new-object status");
    set_last_error(79);
    std::int32_t previous = -7;
    check(release(handle, 2, &previous) == 1 && previous == 0 && wait(handle, 0) == wait_object_0 &&
              wait(handle, 0) == wait_object_0 && wait(handle, 0) == wait_timeout && get_last_error() == 79 &&
              crt_error() == 43,
          "semaphore ABI mutates real permit count and previous output while preserving successful error "
          "channels");
    previous = -7;
    check(release(handle, 3, &previous) == 0 && previous == -7 && get_last_error() == 298 &&
              wait(handle, 0) == wait_timeout && crt_error() == 43,
          "failed over-limit release preserves count/output and reports too-many-posts without altering CRT "
          "errno");
    check(!create(nullptr, 0, 1, nullptr, 1, object_all_access) && get_last_error() == 87 &&
              !create(nullptr, 2, 1, nullptr, 0, object_all_access) && get_last_error() == 87,
          "reserved semaphore flags and invalid ranges fail without creating a kernel object");
    auto *narrow = create_a(nullptr, 0, 1, "semaphore-λ", 0, 0x40000000u);
    auto *wide = create(nullptr, 0, 2, u"Local\\semaphore-λ", 0, 0x20000000u);
    check(narrow && wide && get_last_error() == 183 && release(narrow, 1, nullptr) == 1 &&
              wait(wide, 0) == wait_object_0,
          "UTF-8/UTF-16 named aliases preserve original counts and generic write/execute handle rights");
    previous = -9;
    check(release(wide, 1, &previous) == 0 && get_last_error() == 5 && previous == -9 &&
              wait(narrow, 0) == wait_failed && get_last_error() == 5,
          "semaphore ABI enforces per-handle release/wait rights and leaves failed output untouched");
    auto *all = create(nullptr, 1, 1, nullptr, 0, 0x10000000u);
    check(all && wait(all, 0) == wait_object_0 && release(all, 1, nullptr) == 1,
          "generic-all semaphore access maps to the specific private object rights");
    check(close(handle) == 1 && close(narrow) == 1 && close(wide) == 1 && close(all) == 1 &&
              release(handle, 1, nullptr) == 0 && get_last_error() == 6 && crt_error() == 43,
          "closed semaphore ABI handles cannot release host resources or another runtime object");
}
