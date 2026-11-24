void slist_checks() {
    ambrosia::services::Runtime runtime;
    using Initialize = void(AMBROSIA_GUEST_ABI *)(void *);
    const auto initialize = reinterpret_cast<Initialize>(
        runtime.resolve(service_symbol("InitializeSListHead", "api-ms-win-core-interlocked-l1-1-0.dll")));

    struct alignas(16) Guarded {
        std::array<std::uint64_t, 2> before{0x1234, 0x5678};
        std::array<std::uint64_t, 2> head{UINT64_MAX, UINT64_MAX};
        std::array<std::uint64_t, 2> after{0xabcd, 0xef01};
    } object;

    ambrosia::services::set_last_error(37);
    initialize(object.head.data());
    check(object.head == std::array<std::uint64_t, 2>{0, 1},
          "x64 sequenced-list initialization sets the empty 16-byte header representation");
    check(object.before == std::array<std::uint64_t, 2>{0x1234, 0x5678} &&
              object.after == std::array<std::uint64_t, 2>{0xabcd, 0xef01},
          "sequenced-list initialization preserves adjacent storage");
    check(ambrosia::services::get_last_error() == 37, "sequenced-list initialization preserves last error");
    check(runtime.resolve(service_symbol("InitializeSListHead", "kernel32.dll")) ==
              runtime.resolve(service_symbol("RtlInitializeSListHead", "ntdll.dll")),
          "Win32 and native sequenced-list initialization aliases share one ABI adapter");
}
