unsigned scan_invalid_calls = 0;

void AMBROSIA_GUEST_ABI scan_invalid_parameter(const char16_t *, const char16_t *, const char16_t *,
                                               std::uint32_t, std::uintptr_t) {
    ++scan_invalid_calls;
}

void scan_checks() {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    Scope scope(runtime);
    using Scan = std::int32_t(AMBROSIA_GUEST_ABI *)(std::uint64_t, const char *, std::size_t, const char *,
                                                    void *, const std::byte *);
    const auto address = runtime.resolve(service_symbol("__stdio_common_vsscanf", "ucrtbase.dll"));
    check(address && address == runtime.resolve(service_symbol("__stdio_common_vsscanf",
                                                               "api-ms-win-crt-stdio-l1-1-0.dll")),
          "string scanning aliases share a real six-argument Windows entrypoint");
    if (!address) {
        return;
    }
    const auto scan = reinterpret_cast<Scan>(address);
    const auto call = [&](const char *input, std::size_t count, const char *format,
                          std::initializer_list<std::uint64_t> slots, std::uint64_t options = 2) {
        return scan(options, input, count, format, nullptr,
                    reinterpret_cast<const std::byte *>(slots.begin()));
    };
    const auto pointer = [](auto *value) { return reinterpret_cast<std::uintptr_t>(value); };
    constexpr auto unlimited = std::numeric_limits<std::size_t>::max();
    std::array<char, 32> output{};
    std::array<char, 32> second{};
    const auto saved_errno = errno;
    errno = E2BIG;
    crt_error() = 19;
    set_last_error(87);
    check(call(" \tfirst second", unlimited, "%s%s", {pointer(output.data()), pointer(second.data())}) == 2 &&
              std::string(output.data()) == "first" && std::string(second.data()) == "second",
          "observed scanner options parse actual whitespace-delimited tokens with separate guest slots");
    std::array<char, 6> guarded{'L', 'x', 'x', 'x', 'x', 'R'};
    check(call("abcdef", unlimited, "%3s", {pointer(guarded.data() + 1)}, 0) == 1 &&
              guarded == std::array<char, 6>{'L', 'a', 'b', 'c', 0, 'R'},
          "string width writes the real prefix and one terminator within destination guards");
    const char finite_input[] = {'r', 'a', 'w'};
    check(call(finite_input, sizeof(finite_input), "%hs", {pointer(output.data())}) == 1 &&
              std::string(output.data()) == "raw",
          "finite input need not have a NUL and the narrow h modifier retains byte destinations");
    output.fill('x');
    check(call("a\0hidden", 8, "%s", {pointer(output.data())}) == 1 && output[0] == 'a' && output[1] == 0 &&
              output[2] == 'x',
          "a finite scanning bound still ends at the first input NUL");
    check(call("skip take", unlimited, "%*s %s", {pointer(output.data())}) == 1 &&
              std::string(output.data()) == "take",
          "suppressed conversions consume input without consuming an argument or an assignment");
    check(call("% abc", unlimited, "%% %s", {pointer(output.data())}) == 1 &&
              std::string(output.data()) == "abc" && call("other", unlimited, "!", {}) == 0,
          "escaped percent and ordinary literals match real input and mismatch returns zero");
    output.fill('x');
    check(call(" ", unlimited, "%s", {pointer(output.data())}) == -1 && output[0] == 'x' &&
              call("unread", 0, "%s", {pointer(output.data())}) == -1,
          "input exhaustion before the first conversion reports EOF without changing destinations");
    check(call("one", unlimited, "%s%s", {pointer(output.data()), pointer(second.data())}) == 1 &&
              call("one", unlimited, "%*s%s", {pointer(output.data())}) == 0,
          "later input exhaustion retains assignments and recognizes a completed suppressed conversion");
    output.fill('x');
    check(call(" \nz", unlimited, "%2c%c", {pointer(output.data()), pointer(output.data() + 2)}) == 2 &&
              output[0] == ' ' && output[1] == '\n' && output[2] == 'z' && output[3] == 'x',
          "character fields preserve whitespace and add no terminator");
    output.fill('x');
    check(call("a", unlimited, "%2c", {pointer(output.data())}) == -1 && output[0] == 'x' &&
              call("", unlimited, " \t", {}) == 0,
          "short exact-width character input fails without a partial write and whitespace alone succeeds");
    const char raw_bytes[] = {char(0xc3), char(0xa9), ' ', 'z', 0};
    check(call(raw_bytes, unlimited, "%s", {pointer(output.data())}) == 1 &&
              static_cast<unsigned char>(output[0]) == 0xc3 &&
              static_cast<unsigned char>(output[1]) == 0xa9 && output[2] == 0,
          "narrow scanning copies raw bytes without host locale or UTF-8 reinterpretation");
    check(errno == E2BIG && crt_error() == 19 && get_last_error() == 87,
          "successful and ordinary failed matches preserve host errno, guest errno and LastError");
    {
        guest::ThreadContext context;
        auto activation = context.activate();
        errno = E2BIG;
        crt_error() = 19;
        set_last_error(87);
        runtime.invalid_parameters().replace(&scan_invalid_parameter);
        scan_invalid_calls = 0;
        check(call(nullptr, 0, "%s", {pointer(output.data())}) == -1 &&
                  call("text", unlimited, nullptr, {}) == -1 && call("text", unlimited, "%s", {0}) == -1 &&
                  call("text", unlimited, "%0s", {pointer(output.data())}) == -1 && scan_invalid_calls == 4 &&
                  crt_error() == 22 && errno == E2BIG && get_last_error() == 87,
              "invalid scanning parameters dispatch the guest handler and return EOF with EINVAL");
        runtime.invalid_parameters().replace(nullptr);
    }

    auto caller = fixture();
    text(caller, 0x440, "ucrtbase.dll");
    text(caller, 0x4a2, "__stdio_common_vsscanf");
    text(caller, 0x510, "ab tail");
    text(caller, 0x540, "%3s");
    constexpr unsigned char code[] = {
        0x48, 0x83, 0xec, 0x38, 0xb9, 2, 0,    0,    0,    0x48, 0x8d, 0x15, 0,   0,    0,    0,
        0x41, 0xb8, 16,   0,    0,    0, 0x4c, 0x8d, 0x0d, 0,    0,    0,    0,   0x48, 0xc7, 0x44,
        0x24, 0x20, 0,    0,    0,    0, 0x48, 0x8d, 0x05, 0,    0,    0,    0,   0x48, 0x89, 0x44,
        0x24, 0x28, 0xff, 0x15, 0,    0, 0,    0,    0x48, 0x83, 0xc4, 0x38, 0xc3};
    std::memcpy(caller.data() + 0x200, code, sizeof(code));
    put<std::uint32_t>(caller, 0x20c, 0x2110 - (0x1000 + 16));
    put<std::uint32_t>(caller, 0x219, 0x2140 - (0x1000 + 29));
    put<std::uint32_t>(caller, 0x229, 0x2170 - (0x1000 + 45));
    put<std::uint32_t>(caller, 0x234, 0x2080 - (0x1000 + 56));
    auto image = pe::Image::load(caller, [&](const auto &symbol) { return runtime.resolve(symbol); });
    const auto destination = image.base() + 0x2180;
    std::memcpy(const_cast<std::byte *>(image.base() + 0x2170), &destination, sizeof(destination));
    check(image.run_bootstrap() == 1 && std::strcmp(reinterpret_cast<const char *>(destination), "ab") == 0,
          "an original PE passes scanner locale and argument-slot pointers in the Windows stack positions");
    errno = saved_errno;
}
