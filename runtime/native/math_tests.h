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

template <class Number> void unary_math_checks(ambrosia::services::Runtime &runtime) {
    using Unary = Number(AMBROSIA_GUEST_ABI *)(Number);

    struct Case {
        const char *name;
        Number argument;
        Number expected;
    };

    const auto cases = [] {
        if constexpr (sizeof(Number) == sizeof(float)) {
            return std::array{
                Case{"acosf", 1, 0},   Case{"asinf", 0, 0},      Case{"atanf", 1, 0.7853981633974483},
                Case{"cbrtf", -8, -2}, Case{"ceilf", -1.25, -1}, Case{"cosf", 0, 1},
                Case{"erff", 0, 0},    Case{"expf", 0, 1},       Case{"exp2f", 3, 8},
                Case{"logf", 1, 0},    Case{"log10f", 100, 2},   Case{"log2f", 8, 3},
                Case{"sinf", 0, 0},    Case{"sinhf", 0, 0},      Case{"tanf", 0, 0}};
        } else {
            return std::array{Case{"acos", 1, 0}, Case{"asin", 0, 0}, Case{"atan", 1, 0.7853981633974483},
                              Case{"cos", 0, 1},  Case{"exp", 0, 1},  Case{"exp2", 3, 8},
                              Case{"log", 1, 0},  Case{"log2", 8, 3}, Case{"sin", 0, 0},
                              Case{"tan", 0, 0}};
        }
    }();
    for (const auto &sample : cases) {
        const auto address = runtime.resolve(service_symbol(sample.name, "api-ms-win-crt-math-l1-1-0.dll"));
        check(address && address == runtime.resolve(service_symbol(sample.name, "ucrtbase.dll")) &&
                  address == runtime.resolve(service_symbol(sample.name, "msvcrt.dll")),
              sample.name);
        if (address) {
            const auto result = reinterpret_cast<Unary>(address)(sample.argument);
            check(std::abs(result - sample.expected) <= Number(0.000001), sample.name);
        }
    }
}

template <class Number> void binary_math_checks(ambrosia::services::Runtime &runtime) {
    using Binary = Number(AMBROSIA_GUEST_ABI *)(Number, Number);

    struct Case {
        const char *name;
        Number first;
        Number second;
        Number expected;
    };

    const auto cases = [] {
        if constexpr (sizeof(Number) == sizeof(float)) {
            return std::array{Case{"atan2f", 1, -1, 2.356194490192345}, Case{"fmodf", -7, 4, -3},
                              Case{"powf", 2, 3, 8}, Case{"remainderf", 7, 4, -1},
                              Case{"nextafterf", 0, 1, std::numeric_limits<float>::denorm_min()}};
        } else {
            return std::array{Case{"atan2", 1, -1, 2.356194490192345}, Case{"fmod", -7, 4, -3},
                              Case{"pow", 2, 3, 8}};
        }
    }();
    for (const auto &sample : cases) {
        const auto address = runtime.resolve(service_symbol(sample.name, "api-ms-win-crt-math-l1-1-0.dll"));
        check(address && address == runtime.resolve(service_symbol(sample.name, "ucrtbase.dll")) &&
                  address == runtime.resolve(service_symbol(sample.name, "msvcrt.dll")),
              sample.name);
        if (address) {
            const auto result = reinterpret_cast<Binary>(address)(sample.first, sample.second);
            const auto tolerance = std::abs(sample.expected) * Number(0.000001);
            check(std::abs(result - sample.expected) <= tolerance, sample.name);
        }
    }
}

template <class Number> void split_math_checks(ambrosia::services::Runtime &runtime, const char *name) {
    using Split = Number(AMBROSIA_GUEST_ABI *)(Number, Number *);
    const auto address = runtime.resolve(service_symbol(name, "ucrtbase.dll"));
    check(address && address == runtime.resolve(service_symbol(name, "api-ms-win-crt-math-l1-1-0.dll")) &&
              address == runtime.resolve(service_symbol(name, "msvcrt.dll")),
          "modf aliases preserve the selected floating-point width");
    if (!address) {
        return;
    }
    const auto split = reinterpret_cast<Split>(address);
    std::array<Number, 3> integral{91, 0, 93};
    const auto fraction = split(Number(-3.75), &integral[1]);
    check(fraction == Number(-0.75) && integral[1] == Number(-3) && integral[0] == 91 && integral[2] == 93,
          "modf receives its pointer in RDX, writes exactly one result and retains surrounding guards");
    const auto zero = split(-Number(0), &integral[1]);
    check(zero == 0 && std::signbit(zero) && integral[1] == 0 && std::signbit(integral[1]),
          "modf preserves signed zero in both results");
    const auto infinite = split(-std::numeric_limits<Number>::infinity(), &integral[1]);
    check(infinite == 0 && std::signbit(infinite) && std::isinf(integral[1]) && std::signbit(integral[1]),
          "infinite modf splits into signed zero and infinity without a domain error");
}

void math_family_checks() {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    Scope scope(runtime);
    std::fenv_t saved_environment;
    std::fegetenv(&saved_environment);
    const auto saved_error = errno;
    errno = E2BIG;
    crt_error() = 19;
    set_last_error(87);
    unary_math_checks<float>(runtime);
    unary_math_checks<double>(runtime);
    binary_math_checks<float>(runtime);
    binary_math_checks<double>(runtime);
    split_math_checks<float>(runtime, "modff");
    split_math_checks<double>(runtime, "modf");
    check(errno == E2BIG && crt_error() == 19 && get_last_error() == 87,
          "ordinary math calculations preserve native and guest error state");

    using Unary = double(AMBROSIA_GUEST_ABI *)(double);
    using Binary = double(AMBROSIA_GUEST_ABI *)(double, double);
    const auto logarithm = reinterpret_cast<Unary>(runtime.resolve(service_symbol("log", "ucrtbase.dll")));
    const auto exponential = reinterpret_cast<Unary>(runtime.resolve(service_symbol("exp", "ucrtbase.dll")));
    const auto atan = reinterpret_cast<Unary>(runtime.resolve(service_symbol("atan", "ucrtbase.dll")));
    const auto angle = reinterpret_cast<Binary>(runtime.resolve(service_symbol("atan2", "ucrtbase.dll")));
    const auto remainder = reinterpret_cast<Binary>(runtime.resolve(service_symbol("fmod", "ucrtbase.dll")));
    const auto inf = std::numeric_limits<double>::infinity();
    const auto negative_zero = angle(-0.0, 1.0);
    check(negative_zero == 0 && std::signbit(negative_zero) &&
              std::abs(angle(inf, -inf) - 2.356194490192345) < 0.000000000001 &&
              std::abs(atan(inf) - 1.5707963267948966) < 0.000000000001 && remainder(3, inf) == 3 &&
              crt_error() == 19,
          "atan and atan2 accept infinite inputs; atan2 retains argument order and signed zero");
    std::feclearexcept(FE_ALL_EXCEPT);
    check(std::isnan(logarithm(-1)) && crt_error() == 33 && (std::fetestexcept(FE_INVALID) & FE_INVALID),
          "negative logarithm reports the guest domain error and actual invalid flag");
    crt_error() = 19;
    std::feclearexcept(FE_ALL_EXCEPT);
    const auto pole = logarithm(0);
    check(std::isinf(pole) && std::signbit(pole) && crt_error() == 34 &&
              (std::fetestexcept(FE_DIVBYZERO) & FE_DIVBYZERO),
          "logarithm at zero reports the guest range error and actual divide-by-zero flag");
    crt_error() = 19;
    check(std::isinf(exponential(10000)) && crt_error() == 34,
          "exponential overflow maps native range failure to guest errno");
    check(errno == E2BIG && get_last_error() == 87,
          "math domain and range failures preserve native errno and guest LastError");

    auto caller = fixture();
    text(caller, 0x440, "api-ms-win-crt-math-l1-1-0.dll");
    text(caller, 0x4a2, "atan2f");
    // sub rsp,28h; XMM0=1; XMM1=-1; call IAT; return XMM0's float bits in EAX.
    constexpr unsigned char code[] = {0x48, 0x83, 0xec, 0x28, 0xb8, 0,    0,    0x80, 0x3f, 0x66,
                                      0x0f, 0x6e, 0xc0, 0xb8, 0,    0,    0x80, 0xbf, 0x66, 0x0f,
                                      0x6e, 0xc8, 0xff, 0x15, 0,    0,    0,    0,    0x66, 0x0f,
                                      0x7e, 0xc0, 0x48, 0x83, 0xc4, 0x28, 0xc3};
    std::memcpy(caller.data() + 0x200, code, sizeof(code));
    put<std::uint32_t>(caller, 0x218, 0x2080 - (0x1000 + 28));
    auto image = pe::Image::load(caller, [&](const auto &symbol) { return runtime.resolve(symbol); });
    check(image.run_bootstrap() == 0x4016cbe4,
          "an original PE atan2f import receives XMM0/XMM1 and returns the angle in XMM0");
    std::fesetenv(&saved_environment);
    errno = saved_error;
}
