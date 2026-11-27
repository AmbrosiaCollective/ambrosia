void frame_timing_checks() {
    using namespace ambrosia::services;
    FrameTiming timing;
    constexpr auto invalid = static_cast<std::int32_t>(0x80070057u);
    check(timing.prepare_wait(0, 0, 0, 0, 0).result < 0 && !timing.schedule(0, 0),
          "frame events require a configured timer grid");
    check(timing.set_interval(1000, 3, 100), "frame timer accepts a bounded period and latency");
    const auto configuration = timing.configuration();
    check(!timing.set_interval(0, 3, 200) && !timing.set_interval(1000, 17, 200) &&
              timing.configuration() == configuration,
          "invalid timing changes preserve the current grid");
    check(timing.set_interval(1000, 3, 500) && timing.configuration() == configuration,
          "repeating the same configuration preserves its epoch");
    check(timing.schedule(0, 100) && timing.schedule(2, 250) && !timing.schedule(4, 0) &&
              !timing.schedule(2, 1000001),
          "scheduled event offsets are bounded and invalid changes preserve prior settings");
    const auto current = timing.prepare_wait(0, 0, 2, 999, 200);
    check(current.result == 0 && current.token == 1 && current.wake_us == 200,
          "current-origin polling returns a timer token immediately");
    const auto origin = timing.prepare_wait(0, 0xffffffffu, 0, 999, 200);
    check(origin.result == 0 && origin.token == 2 && origin.wake_us == 1200,
          "blocking origins select the next timer edge and ignore the input token");
    const auto event = timing.prepare_wait(2, 0xffffffffu, 0, 2, 300);
    check(event.result == 0 && event.token == 2 && event.wake_us == 1350,
          "a token-in wait uses that frame's scheduled event offset");
    const auto timeout = timing.prepare_wait(2, 1, 0, 2, 300);
    check(timeout.result == 1 && timeout.token == 2 && timeout.wake_us == 1300,
          "a finite timeout finishes first without changing the caller's token");
    const auto late = timing.prepare_wait(2, 2, 0, 2, 1500);
    check(late.result == 0 && late.token == 2 && late.wake_us <= 1500,
          "an already reached token-in event needs no additional wait");
    check(timing.prepare_wait(1, 0, 0, 0, 300).result == invalid &&
              timing.prepare_wait(0, 0, 1, 0, 300).result == invalid &&
              timing.prepare_wait(2, 0, 2, 0, 300).result == invalid,
          "unscheduled events and unsupported flags are explicitly refused");
    check(timing.prepare_wait(2, 0xffffffffu, 0, UINT64_MAX, 300).result == invalid,
          "token-to-deadline multiplication cannot wrap");
    check(timing.set_interval(2000, 2, 500) && timing.configuration().epoch_us == 500 && event.token == 2 &&
              event.wake_us == 1350,
          "a new grid does not mutate an already prepared wait snapshot");
    check(timing.set_interval(3000, 2, UINT64_MAX - 5) && timing.schedule(0, 10) &&
              timing.prepare_wait(0, 0, 0, 0, UINT64_MAX).result == invalid,
          "overflowing epoch-plus-offset arithmetic is rejected");
}

void frame_timing_live_checks(const std::filesystem::path &root, unsigned probe) {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    Scope scope(runtime);
    guest::ThreadContext context;
    auto activation = context.activate();
    runtime.graphics_devices().bind(root);
    void *device = nullptr;
    check(runtime.graphics_devices().create(&device11_interface, &device, std::nullopt) == 0 && device,
          "frame timing owns a real backend device");
    auto *table = *static_cast<const std::uintptr_t **>(device);
    using Interval =
        std::int32_t(AMBROSIA_GUEST_ABI *)(void *, const void *, std::uint32_t, std::uint32_t, std::uint32_t);
    using Schedule =
        std::int32_t(AMBROSIA_GUEST_ABI *)(void *, std::uint32_t, std::uint32_t, const void *, std::uint32_t);
    using Wait = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, std::uint32_t, std::uint32_t, const void *,
                                                    std::uint32_t, void *);
    using Release = std::uint32_t(AMBROSIA_GUEST_ABI *)(void *);
    const auto interval = reinterpret_cast<Interval>(table[78]);
    const auto schedule = reinterpret_cast<Schedule>(table[79]);
    const auto wait = reinterpret_cast<Wait>(table[80]);
    const auto release = reinterpret_cast<Release>(table[2]);
    std::uint64_t token = 17;
    if (probe == 1) {
        Runtime other;
        Scope foreign(other);
        (void)wait(device, 0, 0, nullptr, 0, &token);
        throw std::runtime_error("foreign frame wait unexpectedly returned");
    }
    if (probe == 2) {
        (void)release(device);
        (void)wait(device, 0, 0, nullptr, 0, &token);
        throw std::runtime_error("released-device frame wait unexpectedly returned");
    }
    check(wait(reinterpret_cast<void *>(1), 0, 0, nullptr, 0, nullptr) == pointer_error,
          "frame wait validates output before device access");
    check(wait(device, 0, 0, nullptr, 0, &token) < 0 && token == 17 && schedule(device, 0, 0, nullptr, 0) < 0,
          "unconfigured timer calls preserve output and refuse scheduling");
    check(interval(device, nullptr, 0, 2, 0) < 0 &&
              interval(device, reinterpret_cast<void *>(1), 2000, 2, 0) < 0 &&
              interval(device, nullptr, 2000, 2, 1) < 0,
          "invalid periods and unimplemented display/flag integration are rejected");
    const auto before_set_us = static_cast<std::uint64_t>(host::performance_counter()) / 1000;
    check(interval(device, nullptr, 2000, 2, 0) == 0, "the interval ABI configures an owned monotonic timer");
    check(schedule(device, 0, 0, nullptr, 0) == 0 && schedule(device, 2, 500, nullptr, 0) == 0,
          "both measured event types retain independent offsets");
    check(schedule(device, 4, 0, nullptr, 0) < 0 &&
              schedule(device, 2, 0, reinterpret_cast<void *>(1), 0) < 0 &&
              schedule(device, 2, 0, nullptr, 1) < 0,
          "unknown event and reserved integration requests are rejected");
    check(wait(device, 1, 0, nullptr, 0, &token) < 0 && token == 17,
          "an unscheduled event fails without modifying output");
    const auto saved_errno = errno;
    errno = E2BIG;
    set_last_error(89);
    crt_error() = 37;
    check(wait(device, 0, 0, nullptr, 2, &token) == 0 && token,
          "nonblocking origin polling returns the current timer token");
    const auto polled = token;
    token = 1000000;
    check(wait(device, 2, 0, nullptr, 0, &token) == 1 && token == 1000000,
          "an immediate event timeout preserves the input token");
    token = 0;
    const auto result = wait(device, 0, 100, nullptr, 0, &token);
    const auto after_origin_us = static_cast<std::uint64_t>(host::performance_counter()) / 1000;
    check(result == 0 && token > polled && after_origin_us >= before_set_us + (token - 1) * 2000,
          "blocking origin waits reach their real monotonic deadline");
    check(wait(device, 2, 100, nullptr, 0, &token) == 0 &&
              static_cast<std::uint64_t>(host::performance_counter()) / 1000 >=
                  before_set_us + (token - 1) * 2000 + 500,
          "the token-in event uses the configured offset and real host wait");
    check(errno == E2BIG && get_last_error() == 89 && crt_error() == 37,
          "frame ABI calls preserve native and guest error state");
    check(release(device) == 0, "timer state releases with its owning real device");
    errno = saved_errno;
}
