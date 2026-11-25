void format_checks() {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    Scope scope(runtime);
    using Format = std::int32_t(AMBROSIA_GUEST_ABI *)(std::uint64_t, char *, std::size_t, const char *,
                                                      void *, const std::byte *);
    const auto address = runtime.resolve(service_symbol("__stdio_common_vsprintf", "ucrtbase.dll"));
    const auto format = reinterpret_cast<Format>(address);
    check(address && address == runtime.resolve(service_symbol("__stdio_common_vsprintf",
                                                               "api-ms-win-crt-stdio-l1-1-0.dll")),
          "buffer formatting aliases share a six-argument Microsoft x64 entrypoint");
    const auto call = [&](std::uint64_t options, char *buffer, std::size_t size, const char *pattern,
                          std::initializer_list<std::uint64_t> slots) {
        return format(options, buffer, size, pattern, nullptr,
                      reinterpret_cast<const std::byte *>(slots.begin()));
    };
    std::array<char, 256> output{};
    const auto saved_error = errno;
    errno = E2BIG;
    crt_error() = 19;
    set_last_error(87);
    check(call(1, output.data(), output.size(), "%ld/%I64d/%I32u/%Iu/%hhd/%hu",
               {0xdeadbeeffffffffeull, 0xfffffffffffffffdull, 0xcafe0000ffffffffull, 0xffffffffffffffffull,
                0xfe, 0x10001}) == 42 &&
              std::string(output.data()) == "-2/-3/4294967295/18446744073709551615/-2/1",
          "integer formatting decodes Windows LLP64 and promoted narrow values without slot-padding leakage");
    check(call(32, output.data(), output.size(), "%+08.2f/%#.0f/%g/%e/%a",
               {std::bit_cast<std::uint64_t>(1.25), std::bit_cast<std::uint64_t>(2.0),
                std::bit_cast<std::uint64_t>(0.125), std::bit_cast<std::uint64_t>(1.0),
                std::bit_cast<std::uint64_t>(1.5)}) > 0 &&
              std::string(output.data()) == "+0001.25/2./0.125/1.000000e+00/0x1.8p+0",
          "floating-point slots are decoded as double and formatted in the invariant numeric locale");
    check(call(1, output.data(), output.size(), "[%*.*s][%.*s]%%",
               {0xfeed0000fffffffauLL, 3, reinterpret_cast<std::uintptr_t>("abcdef"), 0xffffffff,
                reinterpret_cast<std::uintptr_t>("tail")}) == 15 &&
              std::string(output.data()) == "[abc   ][tail]%",
          "negative star width aligns left; negative star precision is omitted and percent consumes no "
          "argument");
    check(call(1, output.data(), output.size(), "%#x/%#.0o/%#.0x/%08.3d", {42, 0, 0, 7}) > 0 &&
              std::string(output.data()) == "0x2a/0//     007",
          "integer precision handles alternate zero octal and overrides zero padding");
    check(call(33, output.data(), output.size(), "%.1f/%g/%g",
               {std::bit_cast<std::uint64_t>(-0.0),
                std::bit_cast<std::uint64_t>(std::numeric_limits<double>::infinity()),
                std::bit_cast<std::uint64_t>(std::numeric_limits<double>::quiet_NaN())}) > 0 &&
              std::string(output.data()) == "-0.0/inf/nan",
          "modern numeric formatting preserves signed zero and non-finite values");
    check(call(1, output.data(), output.size(), "%+.0d/% .0d/%#.0o/%#.0x/%5.0u", {0, 0, 0, 0, 0}) > 0 &&
              std::string(output.data()) == "+/ /0//     ",
          "zero integer precision retains applicable signs, octal prefix and space-only field padding");
    check(call(38, output.data(), output.size(), "%S/%.0f/%.0f",
               {reinterpret_cast<std::uintptr_t>(u"wide"), std::bit_cast<std::uint64_t>(2.5),
                std::bit_cast<std::uint64_t>(3.5)}) > 0 &&
              std::string(output.data()) == "wide/2/4",
          "the observed modern option combination retains narrow-formatter wide arguments and nearest-even "
          "rounding");
    const char raw_bytes[] = {'x', char(0xc3), char(0xa9), 'z', 0};
    check(call(1, output.data(), output.size(), "[%6.3s]", {reinterpret_cast<std::uintptr_t>(raw_bytes)}) ==
                  8 &&
              std::string(output.data()) == std::string("[   x\xc3\xa9]"),
          "narrow string precision and width count raw bytes rather than Unicode display columns");
    check(call(1, output.data(), output.size(), "%ls/%S/%hc/%lc",
               {reinterpret_cast<std::uintptr_t>(u"wide"), reinterpret_cast<std::uintptr_t>(u"other"), 'a',
                'b'}) == 14 &&
              std::string(output.data()) == "wide/other/a/b",
          "wide string and character conversions use Windows UTF-16 with explicit C-locale ASCII coverage");
    const char unterminated[] = {'a', 'b', 'c'};
    check(call(1, output.data(), output.size(), "%.3s/%.0s",
               {reinterpret_cast<std::uintptr_t>(unterminated), 1}) == 4 &&
              std::string(output.data()) == "abc/",
          "precision-bounded strings do not require a terminator or read a zero-precision pointer");
    std::array<char, 6> guarded{'L', 'x', 'x', 'x', 'x', 'R'};
    check(call(2, guarded.data() + 1, 4, "abcdef", {}) == 6 &&
              guarded == std::array<char, 6>{'L', 'a', 'b', 'c', '\0', 'R'},
          "standard snprintf truncation returns full size, terminates the destination and preserves guards");
    guarded.fill('z');
    check(call(2, guarded.data() + 1, 1, "abc", {}) == 3 && guarded[0] == 'z' && guarded[1] == 0 &&
              guarded[2] == 'z' && call(2, nullptr, 0, "%d", {12345}) == 5,
          "one-byte destinations and null zero-size sizing obey standard snprintf bounds");
    check(call(1, output.data(), output.size(), "%cZ", {0}) == 2 && output[0] == 0 && output[1] == 'Z' &&
              output[2] == 0,
          "formatted output retains embedded NUL characters and counts them before its terminating NUL");
    check(errno == E2BIG && crt_error() == 19 && get_last_error() == 87,
          "successful formatted output preserves host errno, guest errno and LastError");

    auto caller = fixture();
    text(caller, 0x440, "ucrtbase.dll");
    text(caller, 0x4a2, "__stdio_common_vsprintf");
    text(caller, 0x540, "%d/%.1f");
    put<std::uint64_t>(caller, 0x570, 42);
    put<std::uint64_t>(caller, 0x578, std::bit_cast<std::uint64_t>(2.5));
    // Six Windows register/stack arguments, with the va_list pointing to guest
    // argument slots. Return the formatter count and inspect its guest buffer.
    constexpr unsigned char code[] = {
        0x48, 0x83, 0xec, 0x38, 0xb9, 32, 0,    0,    0,    0x48, 0x8d, 0x15, 0,   0,    0,    0,
        0x41, 0xb8, 32,   0,    0,    0,  0x4c, 0x8d, 0x0d, 0,    0,    0,    0,   0x48, 0xc7, 0x44,
        0x24, 0x20, 0,    0,    0,    0,  0x48, 0x8d, 0x05, 0,    0,    0,    0,   0x48, 0x89, 0x44,
        0x24, 0x28, 0xff, 0x15, 0,    0,  0,    0,    0x48, 0x83, 0xc4, 0x38, 0xc3};
    std::memcpy(caller.data() + 0x200, code, sizeof(code));
    put<std::uint32_t>(caller, 0x20c, 0x2110 - (0x1000 + 16));
    put<std::uint32_t>(caller, 0x219, 0x2140 - (0x1000 + 29));
    put<std::uint32_t>(caller, 0x229, 0x2170 - (0x1000 + 45));
    put<std::uint32_t>(caller, 0x234, 0x2080 - (0x1000 + 56));
    auto image = pe::Image::load(caller, [&](const auto &symbol) { return runtime.resolve(symbol); });
    check(image.run_bootstrap() == 6 &&
              std::strcmp(reinterpret_cast<const char *>(image.base() + 0x2110), "42/2.5") == 0,
          "an original PE passes locale and Windows argument-slot pointers on the stack to formatted output");
    errno = saved_error;
}
