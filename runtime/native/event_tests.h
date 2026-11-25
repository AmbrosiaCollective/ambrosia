void event_checks() {
    using namespace ambrosia::services;
    EventSignal manual(true, false);
    EventSignal::Waiter first, second, late;
    check(manual.arm(first) == EventSignal::ArmResult::queued &&
              manual.arm(second) == EventSignal::ArmResult::queued,
          "manual event retains unsatisfied waiter registrations");
    manual.signal();
    manual.reset();
    check(first.released && second.released && !manual.poll() &&
              manual.arm(late) == EventSignal::ArmResult::queued && !late.released,
          "manual signal releases existing waiters irrevocably while reset blocks later arrivals");
    manual.cancel(late);
    EventSignal automatic(false, false);
    EventSignal::Waiter selected, remaining;
    automatic.arm(selected);
    automatic.arm(remaining);
    automatic.signal();
    check(selected.released && !remaining.released && !automatic.poll(),
          "auto-reset handoff releases one selected waiter and cannot be stolen by a new poll");
    automatic.reset();
    automatic.signal();
    check(selected.released && remaining.released && !automatic.poll(),
          "reset does not revoke assigned signals and a second signal releases the next waiter");
    automatic.signal();
    automatic.signal();
    check(automatic.poll() && !automatic.poll(),
          "unconsumed auto-reset signals coalesce into one available token");
    EventSignal cancel_test(false, false);
    EventSignal::Waiter cancelled;
    cancel_test.arm(cancelled);
    cancel_test.cancel(cancelled);
    cancel_test.signal();
    check(!cancelled.released && cancel_test.poll(),
          "cancelled waiter leaves no stale pointer or stolen signal");
    EventSignal limited(true, false);
    std::array<EventSignal::Waiter, 65> waiters{};
    bool armed = true;
    for (unsigned index = 0; index < 64; ++index) {
        armed = armed && limited.arm(waiters[index]) == EventSignal::ArmResult::queued;
    }
    check(armed && limited.arm(waiters.back()) == EventSignal::ArmResult::exhausted,
          "event signal model enforces its bounded waiter budget");
    limited.signal();
    check(std::all_of(waiters.begin(), waiters.begin() + 64,
                      [](const auto &waiter) { return waiter.released; }) &&
              !waiters.back().released,
          "manual signal releases the complete registered set without touching rejected waiters");

    KernelObjects objects;
    const auto event = objects.create_event(true, true);
    check(event.handle && !event.error && objects.wait(event.handle, 0).result == wait_object_0 &&
              objects.wait(event.handle, 0).result == wait_object_0,
          "manual-reset object remains signaled across successful polls");
    check(objects.reset_event(event.handle) == 0 && objects.wait(event.handle, 0).result == wait_timeout &&
              objects.wait(event.handle, 1).result == wait_timeout,
          "reset clears event state and finite native condition-variable waits expire");
    check(objects.close(event.handle) == 0 && objects.close(event.handle) == 6 &&
              objects.wait(event.handle, 0).result == wait_failed && objects.set_event(event.handle) == 6,
          "closed event handles cannot be reused to signal, wait or close a host resource");
    const auto named = objects.create_event(false, false, u"shared-event");
    const auto alias = objects.create_event(true, true, u"shared-event");
    check(named.handle && alias.handle && named.handle != alias.handle && alias.error == 183 &&
              objects.wait(alias.handle, 0).result == wait_timeout,
          "opening an existing named event returns a new handle without changing original mode/state");
    check(objects.set_event(alias.handle) == 0 && objects.wait(named.handle, 0).result == wait_object_0 &&
              objects.wait(alias.handle, 0).result == wait_timeout,
          "named event aliases share the actual auto-reset signal state");
    objects.close(named.handle);
    check(objects.set_event(alias.handle) == 0 && objects.wait(alias.handle, 0).result == wait_object_0,
          "closing one alias retains the named event through another handle");
    objects.close(alias.handle);
    const auto recreated = objects.create_event(true, true, u"shared-event");
    check(recreated.handle && !recreated.error && objects.wait(recreated.handle, 0).result == wait_object_0 &&
              objects.wait(recreated.handle, 0).result == wait_object_0,
          "last-close releases named object ownership and a new object uses its new settings");
    KernelObjects bounded(1);
    const auto old = bounded.create_event(false, false);
    check(old.handle && bounded.create_event(false, false).error == 8 && bounded.close(old.handle) == 0,
          "kernel handle budget fails creation without leaking existing ownership");
    const auto newer = bounded.create_event(false, false);
    check(newer.handle && newer.handle != old.handle && bounded.set_event(old.handle) == 6,
          "new event IDs do not resurrect a closed stale handle");

    Runtime runtime;
    Scope scope(runtime);
    using CreateW = void *(AMBROSIA_GUEST_ABI *)(void *, std::int32_t, std::int32_t, const char16_t *);
    using CreateA = void *(AMBROSIA_GUEST_ABI *)(void *, std::int32_t, std::int32_t, const char *);
    using Operation = std::int32_t(AMBROSIA_GUEST_ABI *)(void *);
    using Wait = std::uint32_t(AMBROSIA_GUEST_ABI *)(void *, std::uint32_t);
    const auto create = reinterpret_cast<CreateW>(
        runtime.resolve(service_symbol("CreateEventW", "api-ms-win-core-synch-l1-1-0.dll")));
    const auto create_a =
        reinterpret_cast<CreateA>(runtime.resolve(service_symbol("CreateEventA", "kernel32.dll")));
    const auto signal =
        reinterpret_cast<Operation>(runtime.resolve(service_symbol("SetEvent", "kernel32.dll")));
    const auto reset =
        reinterpret_cast<Operation>(runtime.resolve(service_symbol("ResetEvent", "kernel32.dll")));
    const auto close = reinterpret_cast<Operation>(
        runtime.resolve(service_symbol("CloseHandle", "api-ms-win-core-handle-l1-1-0.dll")));
    const auto wait =
        reinterpret_cast<Wait>(runtime.resolve(service_symbol("WaitForSingleObject", "kernel32.dll")));
    set_last_error(73);
    crt_error() = 17;
    auto *handle = create(nullptr, 1, 0, nullptr);
    check(handle && get_last_error() == 0 && crt_error() == 17,
          "CreateEventW Microsoft ABI owns a new event and clears its creation status");
    set_last_error(81);
    check(signal(handle) == 1 && wait(handle, 0) == wait_object_0 && wait(handle, 0) == wait_object_0 &&
              reset(handle) == 1 && wait(handle, 0) == wait_timeout && get_last_error() == 81,
          "event ABI signal/reset/wait operations change real state while preserving successful last error");
    check(close(handle) == 1 && wait(handle, 0) == wait_failed && get_last_error() == 6 && crt_error() == 17,
          "closed event ABI wait reports WAIT_FAILED/invalid-handle and preserves CRT errno");
    auto *narrow = create_a(nullptr, 0, 0, "event-λ");
    auto *wide = create(nullptr, 1, 1, u"Local\\event-λ");
    check(narrow && wide && get_last_error() == 183 && signal(wide) == 1 &&
              wait(narrow, 0) == wait_object_0 && wait(wide, 0) == wait_timeout,
          "UTF-8/UTF-16 and Local-prefix names share the same private event object");
    close(narrow);
    close(wide);
    const std::u16string oversized_name(260, u'x');
    check(!create(nullptr, 0, 0, oversized_name.c_str()) && get_last_error() == 206 && crt_error() == 17,
          "documented event name limit fails before object creation without changing CRT errno");
}
