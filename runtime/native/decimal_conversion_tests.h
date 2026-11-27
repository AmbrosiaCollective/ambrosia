unsigned decimal_invalid_calls = 0;
bool decimal_invalid_context = false;

void AMBROSIA_GUEST_ABI decimal_invalid_handler(const char16_t *, const char16_t *, const char16_t *,
                                                std::uint32_t, std::uintptr_t) {
    ++decimal_invalid_calls;
    decimal_invalid_context = ambrosia::guest::ThreadContext::current() != nullptr;
    ambrosia::services::crt_error() = 91;
    errno = EDOM;
}

void decimal_conversion_checks() {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    Scope scope(runtime);
    guest::ThreadContext context;
    auto activation = context.activate();
    using Convert = std::int32_t(AMBROSIA_GUEST_ABI *)(const char *);
    const auto address = runtime.resolve(service_symbol("atoi", "api-ms-win-crt-convert-l1-1-0.dll"));
    check(address && address == runtime.resolve(service_symbol("atoi", "ucrtbase.dll")) &&
              address == runtime.resolve(service_symbol("atoi", "msvcrt.dll")),
          "CRT decimal aliases share one Windows x64 integer conversion service");
    if (!address) {
        return;
    }
    const auto convert = reinterpret_cast<Convert>(address);
    const auto saved_error = errno;
    errno = E2BIG;
    crt_error() = 19;
    set_last_error(83);
    check(convert(" \t\n\v\f\r+123tail") == 123 && convert("-42.5") == -42 && convert("0077") == 77 &&
              convert("0x20") == 0,
          "C-locale whitespace and optional sign precede decimal digits with no base inference");
    const char unterminated[] = {'4', '2', 'x'};
    check(convert(unterminated) == 42 && convert("") == 0 && convert("word") == 0 && convert("+") == 0 &&
              convert("- 5") == 0 &&
              convert("\xa0"
                      "12") == 0,
          "decimal conversion stops at the first non-digit without requiring a later string terminator");
    check(convert("2147483647") == INT32_MAX && convert("-2147483648") == INT32_MIN && convert("-0") == 0 &&
              errno == E2BIG && crt_error() == 19 && get_last_error() == 83,
          "exact Windows int boundaries and ordinary conversion preserve host and guest errors");
    check(convert("2147483648") == INT32_MAX && crt_error() == 34 && convert("-2147483649") == INT32_MIN &&
              crt_error() == 34,
          "Windows int overflow saturates in either direction and sets guest ERANGE");
    const std::string large(512, '9');
    check(convert(large.c_str()) == INT32_MAX && convert(("-" + large).c_str()) == INT32_MIN &&
              crt_error() == 34 && errno == E2BIG && get_last_error() == 83,
          "overflow beyond the native unsigned range remains bounded and preserves unrelated errors");
    const std::string leading_zeroes(1024, '0');
    check(convert((leading_zeroes + "00123suffix").c_str()) == 123 && crt_error() == 34,
          "leading zeroes do not cause overflow or clear a prior guest errno");
    decimal_invalid_calls = 0;
    decimal_invalid_context = false;
    runtime.invalid_parameters().replace(&decimal_invalid_handler);
    check(convert(nullptr) == 0 && crt_error() == 22 && decimal_invalid_calls == 1 &&
              decimal_invalid_context && errno == E2BIG && get_last_error() == 83,
          "null decimal input dispatches the guest handler then returns EINVAL while preserving host errno");
    auto bytes = fixture();
    text(bytes, 0x440, "ucrtbase.dll");
    text(bytes, 0x4a2, "atoi");
    text(bytes, 0x540, " \t+271tail");
    constexpr unsigned char code[] = {0x48, 0x83, 0xec, 0x28, 0x48, 0x8d, 0x0d, 0,    0,    0,    0,
                                      0xff, 0x15, 0,    0,    0,    0,    0x48, 0x83, 0xc4, 0x28, 0xc3};
    std::memcpy(bytes.data() + 0x200, code, sizeof(code));
    put<std::uint32_t>(bytes, 0x207, 0x2140 - (0x1000 + 11));
    put<std::uint32_t>(bytes, 0x20d, 0x2080 - (0x1000 + 17));
    auto image = pe::Image::load(bytes, [&](const auto &symbol) { return runtime.resolve(symbol); });
    check(image.run_bootstrap() == 271 &&
              std::strcmp(reinterpret_cast<const char *>(image.base() + 0x2140), " \t+271tail") == 0,
          "an original PE passes a guest decimal string through the Windows import and receives its value");
    errno = saved_error;
}

int decimal_rejection_probe(unsigned mode) {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    Scope scope(runtime);
    using Convert = std::int32_t(AMBROSIA_GUEST_ABI *)(const char *);
    const auto convert = reinterpret_cast<Convert>(runtime.resolve(service_symbol("atoi", "ucrtbase.dll")));
    guest::ThreadContext context;
    auto activation = context.activate();
    if (mode == 1) {
        const std::string exhausted(32768, '0');
        (void)convert(exhausted.c_str());
    } else if (mode == 2) {
        (void)convert(nullptr);
    } else {
        return 2;
    }
    return 1;
}
