void secure_random_checks() {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    Scope scope(runtime);
    using Random = std::int32_t(AMBROSIA_GUEST_ABI *)(void *);
    const auto address = runtime.resolve(service_symbol("rand_s", "api-ms-win-crt-utility-l1-1-0.dll"));
    const auto random = reinterpret_cast<Random>(address);
    check(address && address == runtime.resolve(service_symbol("rand_s", "ucrtbase.dll")) &&
              address == runtime.resolve(service_symbol("rand_s", "msvcrt.dll")),
          "secure CRT random aliases share a callable Microsoft x64 unsigned-int output facade");
    const auto saved_error = errno;
    errno = E2BIG;
    crt_error() = 19;
    set_last_error(87);
    std::array<std::byte, 8> output;
    output.fill(std::byte{0xcc});
    check(random(output.data() + 2) == 0 && output[0] == std::byte{0xcc} && output[1] == std::byte{0xcc} &&
              output[6] == std::byte{0xcc} && output[7] == std::byte{0xcc},
          "rand_s fills exactly four possibly unaligned bytes while retaining destination guards");
    check(errno == E2BIG && crt_error() == 19 && get_last_error() == 87,
          "successful OS entropy queries preserve native errno and both guest error domains");
    // No entropy-quality or nonzero-value assertion: zero/repeated values are
    // valid random outputs. OS-source fidelity is checked at the host boundary.
    bool filled = true;
    for (unsigned i = 0; i < 32; ++i) {
        std::uint32_t sample;
        filled &= random(&sample) == 0;
    }
    check(filled, "repeated CRT random calls fill caller-owned values without changing CRT PRNG state");
    auto caller = fixture();
    text(caller, 0x440, "ucrtbase.dll");
    text(caller, 0x4a2, "rand_s");
    constexpr unsigned char code[] = {0x48, 0x83, 0xec, 0x28, 0x48, 0x8d, 0x0d, 0,    0,    0,    0,
                                      0xff, 0x15, 0,    0,    0,    0,    0x48, 0x83, 0xc4, 0x28, 0xc3};
    std::memcpy(caller.data() + 0x200, code, sizeof(code));
    put<std::uint32_t>(caller, 0x207, 0x2112 - (0x1000 + 11));
    put<std::uint32_t>(caller, 0x20d, 0x2080 - (0x1000 + 17));
    std::fill(caller.begin() + 0x510, caller.begin() + 0x518, std::byte{0xcc});
    auto image = pe::Image::load(caller, [&](const auto &symbol) { return runtime.resolve(symbol); });
    check(image.run_bootstrap() == 0 && image.base()[0x2110] == std::byte{0xcc} &&
              image.base()[0x2111] == std::byte{0xcc} && image.base()[0x2116] == std::byte{0xcc} &&
              image.base()[0x2117] == std::byte{0xcc},
          "an original PE rand_s import writes its guest buffer and returns the CRT status in EAX");
    errno = saved_error;
}
