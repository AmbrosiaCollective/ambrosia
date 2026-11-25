void clock_service_checks() {
    using namespace ambrosia;
    constexpr std::uint64_t epoch = 116444736000000000;
    check(host::filetime_from_unix_ticks(0) == epoch, "FILETIME translates the Unix epoch to 1601 units");
    check(host::filetime_from_unix_ticks(1) == epoch + 1 && host::filetime_from_unix_ticks(-1) == epoch - 1,
          "FILETIME preserves exact 100 ns steps on both sides of Unix epoch");
    check(host::filetime_from_unix_ticks(-static_cast<std::int64_t>(epoch)) == 0,
          "FILETIME epoch itself is representable");
    check(runtime_rejected([&] { (void)host::filetime_from_unix_ticks(INT64_MIN); }),
          "FILETIME conversion rejects pre-1601 times without negation overflow");
    check(host::filetime_from_unix_ticks(INT64_MAX) == epoch + static_cast<std::uint64_t>(INT64_MAX),
          "FILETIME conversion avoids signed overflow for large positive input");
    services::Runtime runtime;
    using GetTime = void(AMBROSIA_GUEST_ABI *)(void *);
    using Query = std::int32_t(AMBROSIA_GUEST_ABI *)(void *);
    using GetId = std::uint32_t(AMBROSIA_GUEST_ABI *)();
    const auto get_time =
        reinterpret_cast<GetTime>(runtime.resolve(service_symbol("GetSystemTimeAsFileTime")));
    const auto counter = reinterpret_cast<Query>(runtime.resolve(service_symbol("QueryPerformanceCounter")));
    const auto frequency =
        reinterpret_cast<Query>(runtime.resolve(service_symbol("QueryPerformanceFrequency")));
    const auto process = reinterpret_cast<GetId>(runtime.resolve(service_symbol("GetCurrentProcessId")));
    const auto thread = reinterpret_cast<GetId>(runtime.resolve(service_symbol("GetCurrentThreadId")));
    check(runtime.resolve(service_symbol("GetSystemTimeAsFileTime", "api-ms-win-core-sysinfo-l1-1-0.dll")) ==
              reinterpret_cast<std::uintptr_t>(get_time),
          "observed system-time API-set alias reaches the native clock adapter");
    check(runtime.resolve(service_symbol("QueryPerformanceCounter", "api-ms-win-core-profile-l1-1-0.dll")) ==
              reinterpret_cast<std::uintptr_t>(counter),
          "performance API-set alias resolves explicitly");
    std::array<std::byte, 16> output;
    output.fill(std::byte{0xcc});
    services::set_last_error(55);
    const auto before = host::utc_filetime();
    get_time(output.data() + 4);
    const auto after = host::utc_filetime();
    std::uint64_t value = 0;
    std::memcpy(&value, output.data() + 4, sizeof(value));
    check(value >= std::min(before, after) && value <= std::max(before, after),
          "guest time wrapper writes the current UTC FILETIME value");
    check(output[0] == std::byte{0xcc} && output[3] == std::byte{0xcc} && output[12] == std::byte{0xcc} &&
              output[15] == std::byte{0xcc},
          "four-byte-aligned FILETIME output touches exactly eight bytes");
    check(services::get_last_error() == 55, "successful clock query preserves guest last error");
    std::int64_t rate = 0, first = 0, second = 0;
    check(frequency(&rate) == 1 && rate == 1000000000,
          "performance frequency is a fixed nanosecond unit in counts per second");
    check(counter(&first) == 1 && counter(&second) == 1 && second >= first,
          "performance counter follows a monotonic host clock");
    check(!counter(nullptr) && services::get_last_error() == services::error_invalid_parameter,
          "null performance output fails explicitly");
    check(!frequency(nullptr), "null frequency output fails explicitly");
    services::set_last_error(33);
    check(process() != 0 && process() == host::current_process_id(),
          "process ID wrapper returns the current isolated process identity");
    check(thread() != 0 && thread() == host::current_thread_id() && thread() == thread(),
          "thread ID wrapper is stable and matches the native calling thread");
    check(services::get_last_error() == 33, "successful identity queries preserve last error");

    auto bytes = fixture();
    put<std::uint32_t>(bytes, 0x40c, 0x20c0);
    text(bytes, 0x4c0, "api-ms-win-core-sysinfo-l1-1-0.dll");
    text(bytes, 0x4a2, "GetSystemTimeAsFileTime");
    const unsigned char entry[] = {0x48, 0x83, 0xec, 0x28, 0x48, 0x8d, 0x0d, 0,    0,    0,
                                   0,    0xff, 0x15, 0,    0,    0,    0,    0x48, 0x8b, 0x05,
                                   0,    0,    0,    0,    0x48, 0x83, 0xc4, 0x28, 0xc3};
    std::memcpy(bytes.data() + 0x200, entry, sizeof(entry));
    put<std::uint32_t>(bytes, 0x207, 0x21f0 - 0x100b);
    put<std::uint32_t>(bytes, 0x20d, 0x2080 - 0x1011);
    put<std::uint32_t>(bytes, 0x214, 0x21f0 - 0x1018);
    auto image = pe::Image::load(bytes, [&](const auto &symbol) { return runtime.resolve(symbol); });
    const auto guest_value = image.run_bootstrap();
    check(guest_value >= before && guest_value - before < 10000000,
          "synthetic PE calls the real FILETIME adapter through Microsoft ABI and guest output memory");
}

void crt_time_checks() {
    using namespace ambrosia;
    using namespace services;
    constexpr std::uint64_t epoch = 116444736000000000;
    constexpr std::uint64_t maximum = 0x793406fff;
    check(crt_seconds_from_filetime(epoch - 1) == -1 && crt_seconds_from_filetime(epoch) == 0 &&
              crt_seconds_from_filetime(epoch + 9999999) == 0 &&
              crt_seconds_from_filetime(epoch + 10000000) == 1,
          "CRT UTC conversion checks the Unix epoch before truncating FILETIME subsecond ticks");
    check(
        crt_seconds_from_filetime(epoch + maximum * 10000000) == static_cast<std::int64_t>(maximum) &&
            crt_seconds_from_filetime(epoch + (maximum + 1) * 10000000 - 1) ==
                static_cast<std::int64_t>(maximum) &&
            crt_seconds_from_filetime(epoch + (maximum + 1) * 10000000) ==
                static_cast<std::int64_t>(maximum + 1) &&
            crt_seconds_from_filetime(UINT64_MAX) ==
                static_cast<std::int64_t>((UINT64_MAX - epoch) / 10000000),
        "CRT UTC conversion has no local-calendar cutoff and avoids overflow across the full FILETIME range");
    Runtime runtime;
    Scope scope(runtime);
    using Time = std::int64_t(AMBROSIA_GUEST_ABI *)(void *);
    using Difference = double(AMBROSIA_GUEST_ABI *)(std::int64_t, std::int64_t);
    const auto address = runtime.resolve(service_symbol("_time64", "api-ms-win-crt-time-l1-1-0.dll"));
    const auto time = reinterpret_cast<Time>(address);
    const auto difference =
        reinterpret_cast<Difference>(runtime.resolve(service_symbol("_difftime64", "ucrtbase.dll")));
    check(address && address == runtime.resolve(service_symbol("_time64", "ucrtbase.dll")) &&
              address == runtime.resolve(service_symbol("_time64", "msvcrt.dll")) &&
              reinterpret_cast<std::uintptr_t>(difference) ==
                  runtime.resolve(service_symbol("_difftime64", "api-ms-win-crt-time-l1-1-0.dll")),
          "CRT UTC and difference aliases preserve explicit Windows int64 and double signatures");
    const auto saved_error = errno;
    errno = E2BIG;
    crt_error() = 17;
    set_last_error(83);
    std::array<std::byte, 12> output;
    output.fill(std::byte{0xcc});
    const auto before = crt_seconds_from_filetime(host::utc_filetime());
    const auto now = time(output.data() + 2);
    const auto after = crt_seconds_from_filetime(host::utc_filetime());
    std::int64_t stored = -1;
    std::memcpy(&stored, output.data() + 2, sizeof(stored));
    check(now >= std::min(before, after) && now <= std::max(before, after) && stored == now &&
              output[0] == std::byte{0xcc} && output[1] == std::byte{0xcc} && output[10] == std::byte{0xcc} &&
              output[11] == std::byte{0xcc},
          "CRT time returns real UTC seconds and writes exactly eight output bytes without borrowing native "
          "time_t");
    const auto optional = time(nullptr);
    check(optional >= std::min(before, after) && optional <= crt_seconds_from_filetime(host::utc_filetime()),
          "a null CRT time output is a supported query rather than an invalid parameter");
    check(difference(100, 97) == 3 && difference(97, 100) == -3 && difference(0, 0) == 0 &&
              difference(maximum, maximum - 1) == 1 &&
              difference(0, maximum) == -static_cast<double>(maximum),
          "CRT time differences retain argument order and one-second precision across the supported range");
    check(errno == E2BIG && crt_error() == 17 && get_last_error() == 83,
          "successful UTC time and difference calls preserve native errno and both guest error domains");
    auto caller = fixture();
    text(caller, 0x440, "ucrtbase.dll");
    text(caller, 0x4a2, "_time64");
    constexpr unsigned char code[] = {0x48, 0x83, 0xec, 0x28, 0x31, 0xc9, 0xff, 0x15, 0,
                                      0,    0,    0,    0x48, 0x83, 0xc4, 0x28, 0xc3};
    std::memcpy(caller.data() + 0x200, code, sizeof(code));
    put<std::uint32_t>(caller, 0x208, 0x2080 - (0x1000 + 12));
    auto image = pe::Image::load(caller, [&](const auto &symbol) { return runtime.resolve(symbol); });
    const auto imported = image.run_bootstrap();
    check(imported >= static_cast<std::uint64_t>(std::min(before, after)) &&
              imported <= static_cast<std::uint64_t>(crt_seconds_from_filetime(host::utc_filetime())),
          "an original PE _time64 import returns full UTC epoch seconds in RAX");
    errno = saved_error;
}

void uptime_checks() {
    using namespace ambrosia;
    using namespace services;
    check(host::uptime_from_parts(0, 0) == 0 && host::uptime_from_parts(1, 999999) == 1000 &&
              host::uptime_from_parts(1, 1000000) == 1001 && host::uptime_from_parts(1, 999999999) == 1999,
          "uptime conversion counts full milliseconds and truncates submillisecond fractions");
    check(runtime_rejected([] { (void)host::uptime_from_parts(-1, 0); }) &&
              runtime_rejected([] { (void)host::uptime_from_parts(0, -1); }) &&
              runtime_rejected([] { (void)host::uptime_from_parts(0, 1000000000); }) &&
              runtime_rejected([] { (void)host::uptime_from_parts(INT64_MAX, 0); }),
          "negative, malformed and overflowing uptime clock values fail instead of wrapping");
    check(host::uptime_from_parts(4294967, 295000000) == UINT32_MAX &&
              host::uptime_from_parts(4294967, 296000000) == std::uint64_t(UINT32_MAX) + 1,
          "host uptime retains milliseconds beyond the 32-bit forty-nine-day boundary");
    Runtime runtime;
    Scope scope(runtime);
    using Tick64 = std::uint64_t(AMBROSIA_GUEST_ABI *)();
    using Tick32 = std::uint32_t(AMBROSIA_GUEST_ABI *)();
    const auto address =
        runtime.resolve(service_symbol("GetTickCount64", "api-ms-win-core-sysinfo-l1-1-0.dll"));
    const auto tick64 = reinterpret_cast<Tick64>(address);
    const auto tick32 =
        reinterpret_cast<Tick32>(runtime.resolve(service_symbol("GetTickCount", "kernel32.dll")));
    check(address && address == runtime.resolve(service_symbol("GetTickCount64", "kernel32.dll")) &&
              address == runtime.resolve(service_symbol("GetTickCount64", "kernelbase.dll")) &&
              reinterpret_cast<std::uintptr_t>(tick32) ==
                  runtime.resolve(service_symbol("GetTickCount", "api-ms-win-core-sysinfo-l1-1-0.dll")),
          "uptime APIs expose distinct Windows-width facades through kernel and sysinfo aliases");
    const auto saved_error = errno;
    errno = E2BIG;
    crt_error() = 17;
    set_last_error(83);
    const auto before = host::uptime_milliseconds();
    const auto first = tick64();
    const auto narrow = tick32();
    const auto second = tick64();
    const auto after = host::uptime_milliseconds();
    check(first >= before && second >= first && after >= second,
          "guest uptime follows the native boot-time clock monotonically rather than starting at process "
          "launch");
    check(static_cast<std::uint32_t>(std::uint64_t(narrow) - static_cast<std::uint32_t>(first)) <=
              after - before,
          "32-bit uptime follows the same millisecond clock with modular-width arithmetic");
    check(errno == E2BIG && crt_error() == 17 && get_last_error() == 83,
          "both uptime calls preserve native errno and guest error state");
    auto caller = fixture();
    text(caller, 0x440, "kernel32.dll");
    text(caller, 0x4a2, "GetTickCount64");
    constexpr unsigned char code[] = {0x48, 0x83, 0xec, 0x28, 0xff, 0x15, 0,   0,
                                      0,    0,    0x48, 0x83, 0xc4, 0x28, 0xc3};
    std::memcpy(caller.data() + 0x200, code, sizeof(code));
    put<std::uint32_t>(caller, 0x206, 0x2080 - (0x1000 + 10));
    auto image = pe::Image::load(caller, [&](const auto &symbol) { return runtime.resolve(symbol); });
    const auto imported = image.run_bootstrap();
    check(imported >= first && imported <= host::uptime_milliseconds(),
          "an original PE GetTickCount64 import returns system uptime in full-width RAX");
    errno = saved_error;
}
