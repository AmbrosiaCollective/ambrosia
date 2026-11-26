void precise_time_sleep_checks() {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    using Time = void(AMBROSIA_GUEST_ABI *)(void *);
    using Sleep = void(AMBROSIA_GUEST_ABI *)(std::uint32_t);
    const auto precise = reinterpret_cast<Time>(
        runtime.resolve(service_symbol("GetSystemTimePreciseAsFileTime", "kernel32.dll")));
    const auto sleep = reinterpret_cast<Sleep>(runtime.resolve(service_symbol("Sleep", "kernel32.dll")));
    check(runtime.resolve(
              service_symbol("GetSystemTimePreciseAsFileTime", "api-ms-win-core-sysinfo-l1-2-0.dll")) ==
                  reinterpret_cast<std::uintptr_t>(precise) &&
              runtime.resolve(service_symbol("Sleep", "api-ms-win-core-synch-l1-2-0.dll")) ==
                  reinterpret_cast<std::uintptr_t>(sleep),
          "measured precise-time and sleep API contracts resolve the typed native services");
    std::array<std::byte, 16> output;
    output.fill(std::byte{0xcc});
    const auto saved_error = errno;
    errno = EDOM;
    crt_error() = 61;
    set_last_error(91);
    const auto before = host::utc_filetime();
    precise(output.data() + 3);
    const auto after = host::utc_filetime();
    std::uint64_t value = 0;
    std::memcpy(&value, output.data() + 3, sizeof(value));
    check(value >= std::min(before, after) && value <= std::max(before, after) &&
              output[2] == std::byte{0xcc} && output[11] == std::byte{0xcc} && errno == EDOM &&
              crt_error() == 61 && get_last_error() == 91,
          "precise UTC writes exactly eight unaligned FILETIME bytes from the real native clock while "
          "preserving caller errors");
    precise(nullptr);
    check(get_last_error() == 87 && errno == EDOM && crt_error() == 61,
          "null precise-time output is rejected without a write or native-error leak");
    set_last_error(91);
    sleep(0);
    const auto start = std::chrono::steady_clock::now();
    sleep(20);
    const auto duration = std::chrono::steady_clock::now() - start;
    check(duration >= std::chrono::milliseconds(20) && errno == EDOM && crt_error() == 61 &&
              get_last_error() == 91,
          "zero sleep yields and finite native sleep does not return early or alter guest error state");
    auto bytes = fixture();
    text(bytes, 0x440, "kernel32.dll");
    text(bytes, 0x4a2, "GetSystemTimePreciseAsFileTime");
    const unsigned char code[] = {0x48, 0x83, 0xec, 0x28, 0x48, 0x8d, 0x0d, 0,    0,    0,
                                  0,    0xff, 0x15, 0,    0,    0,    0,    0x48, 0x8b, 0x05,
                                  0,    0,    0,    0,    0x48, 0x83, 0xc4, 0x28, 0xc3};
    std::memcpy(bytes.data() + 0x200, code, sizeof(code));
    put<std::uint32_t>(bytes, 0x207, 0x21f0 - 0x100b);
    put<std::uint32_t>(bytes, 0x20d, 0x2080 - 0x1011);
    put<std::uint32_t>(bytes, 0x214, 0x21f0 - 0x1018);
    auto image = pe::Image::load(bytes, [&](const auto &symbol) { return runtime.resolve(symbol); });
    const auto guest_time = image.run_bootstrap();
    check(guest_time >= before && guest_time <= host::utc_filetime(),
          "original PE entry receives precise native UTC through Microsoft ABI and its own output buffer");
    errno = saved_error;
}
