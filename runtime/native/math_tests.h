void tangent_checks() {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    Scope scope(runtime);
    using Tangent = float(AMBROSIA_GUEST_ABI *)(float);
    const auto address = runtime.resolve(service_symbol("tanf", "api-ms-win-crt-math-l1-1-0.dll"));
    const auto tangent = reinterpret_cast<Tangent>(address);
    check(address && address == runtime.resolve(service_symbol("tanf", "ucrtbase.dll")) &&
              address == runtime.resolve(service_symbol("tanf", "msvcrt.dll")),
          "float tangent aliases share a callable native facade");
    std::fenv_t saved_environment;
    std::fegetenv(&saved_environment);
    const auto saved_error = errno;
    errno = E2BIG;
    set_last_error(83);
    crt_error() = 17;
    const auto positive_zero = tangent(0.0f);
    const auto negative_zero = tangent(-0.0f);
    check(positive_zero == 0 && !std::signbit(positive_zero) && negative_zero == 0 &&
              std::signbit(negative_zero),
          "float tangent retains both signs of zero");
    const auto positive = tangent(0.785398185253143310546875f);
    const auto negative = tangent(-0.785398185253143310546875f);
    check(std::abs(positive - 1.0f) < 0.000001f && std::abs(negative + 1.0f) < 0.000001f,
          "float tangent calculates analytic quarter-pi values through XMM arguments and returns");
    check(errno == E2BIG && crt_error() == 17 && get_last_error() == 83,
          "successful tangent preserves native errno, guest CRT errno and Win32 LastError");
    std::feclearexcept(FE_ALL_EXCEPT);
    const auto infinite = tangent(std::numeric_limits<float>::infinity());
    check(std::isnan(infinite) && crt_error() == 33 && errno == E2BIG && get_last_error() == 83 &&
              (std::fetestexcept(FE_INVALID) & FE_INVALID),
          "infinite tangent produces NaN, guest domain errno and the masked invalid floating-point flag");
    std::feclearexcept(FE_ALL_EXCEPT);
    crt_error() = 17;
    const auto quiet = tangent(std::numeric_limits<float>::quiet_NaN());
    check(std::isnan(quiet) && crt_error() == 33 && errno == E2BIG && !std::fetestexcept(FE_INVALID),
          "quiet NaN follows default CRT domain policy without fabricating an invalid floating-point flag");
    auto caller = fixture();
    text(caller, 0x440, "ucrtbase.dll");
    text(caller, 0x4a2, "tanf");
    // sub rsp,28h; load pi/4 in XMM0; call IAT; return XMM0's float bits in EAX.
    constexpr unsigned char code[] = {0x48, 0x83, 0xec, 0x28, 0xb8, 0xdb, 0x0f, 0x49, 0x3f, 0x66,
                                      0x0f, 0x6e, 0xc0, 0xff, 0x15, 0,    0,    0,    0,    0x66,
                                      0x0f, 0x7e, 0xc0, 0x48, 0x83, 0xc4, 0x28, 0xc3};
    std::memcpy(caller.data() + 0x200, code, sizeof(code));
    put<std::uint32_t>(caller, 0x20f, 0x2080 - (0x1000 + 19));
    auto image = pe::Image::load(caller, [&](const auto &symbol) { return runtime.resolve(symbol); });
    check(image.run_bootstrap() == 0x3f800000,
          "an original PE import passes a float in XMM0 and receives the calculated float in XMM0");
    std::fesetenv(&saved_environment);
    errno = saved_error;
}
