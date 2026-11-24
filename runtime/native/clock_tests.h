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
