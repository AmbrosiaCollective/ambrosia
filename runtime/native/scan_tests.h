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

void scan_integer_checks() {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    Scope scope(runtime);
    using Scan = std::int32_t(AMBROSIA_GUEST_ABI *)(std::uint64_t, const char *, std::size_t, const char *,
                                                    void *, const std::byte *);
    const auto scan =
        reinterpret_cast<Scan>(runtime.resolve(service_symbol("__stdio_common_vsscanf", "ucrtbase.dll")));
    const auto call = [&](const char *input, const char *format, std::initializer_list<std::uint64_t> slots) {
        return scan(2, input, std::numeric_limits<std::size_t>::max(), format, nullptr,
                    reinterpret_cast<const std::byte *>(slots.begin()));
    };
    const auto pointer = [](auto *value) { return reinterpret_cast<std::uintptr_t>(value); };
    const auto saved_errno = errno;
    errno = E2BIG;
    crt_error() = 19;
    set_last_error(87);
    std::int32_t first = 0;
    std::int32_t second = 0;
    check(call(" \t-42 +017", "%d%d", {pointer(&first), pointer(&second)}) == 2 && first == -42 &&
              second == 17,
          "observed decimal scanning parses actual signed values and retains decimal leading zeroes");
    first = 123;
    check(call("letters", "%d", {pointer(&first)}) == 0 && first == 123 &&
              call("-", "%d", {pointer(&first)}) == 0 && first == 123,
          "numeric matching failures make no assignment, including a sign without digits");
    check(call("", "%d", {pointer(&first)}) == -1 &&
              call("12", "%d%d", {pointer(&first), pointer(&second)}) == 1 && first == 12 &&
              call("12", "%*d%d", {pointer(&first)}) == 0,
          "integer input exhaustion distinguishes the first and later completed conversions");
    char trailing = 0;
    check(call("-12X", "%3d%c", {pointer(&first), pointer(&trailing)}) == 2 && first == -12 &&
              trailing == 'X',
          "numeric widths include the sign and leave following bytes for the next conversion");
    std::int8_t tiny = 0;
    std::int16_t small = 0;
    std::array<std::int64_t, 6> large{};
    check(call("-128 -32768 -2147483648 2147483647 -9223372036854775808 9223372036854775807 -7 -8 -9 -10",
               "%hhd %hd %ld %I32d %lld %I64d %jd %zd %td %Id",
               {pointer(&tiny), pointer(&small), pointer(&first), pointer(&second), pointer(&large[0]),
                pointer(&large[1]), pointer(&large[2]), pointer(&large[3]), pointer(&large[4]),
                pointer(&large[5])}) == 10 &&
              tiny == -128 && small == -32768 && first == INT32_MIN && second == INT32_MAX &&
              large == std::array<std::int64_t, 6>{INT64_MIN, INT64_MAX, -7, -8, -9, -10},
          "signed scanning uses Windows LLP64 destination widths and exact 64-bit limits");
    std::uint8_t byte_value = 0;
    std::uint16_t short_value = 0;
    std::uint32_t long_value = 0;
    std::uint64_t wide_value = 0;
    check(call("255 65535 4294967295 18446744073709551615", "%hhu %hu %lu %I64u",
               {pointer(&byte_value), pointer(&short_value), pointer(&long_value), pointer(&wide_value)}) ==
                  4 &&
              byte_value == UINT8_MAX && short_value == UINT16_MAX && long_value == UINT32_MAX &&
              wide_value == UINT64_MAX,
          "unsigned scanning stores full-width maxima without a host long or signed intermediate");
    std::array<std::uint32_t, 3> hex{};
    std::int32_t third = 0;
    check(call("017 0x2a -23 17 0xff FF", "%i %i %i %o %x %X",
               {pointer(&first), pointer(&second), pointer(&third), pointer(&hex[0]), pointer(&hex[1]),
                pointer(&hex[2])}) == 6 &&
              first == 15 && second == 42 && third == -23 && hex[0] == 15 && hex[1] == 255 && hex[2] == 255,
          "auto-base, octal and hexadecimal scanning accept real prefixes and stop at field boundaries");
    check(call("-1 -0x2", "%u %x", {pointer(&long_value), pointer(&hex[0])}) == 2 &&
              long_value == UINT32_MAX && hex[0] == UINT32_MAX - 1,
          "negative unsigned conversions negate the parsed magnitude at the Windows destination width");
    check(call("12:34", "%*d:%d", {pointer(&first)}) == 1 && first == 34,
          "suppressed integer fields consume no guest destination slot or assignment count");
    second = 456;
    check(call("12,none", "%d,%d", {pointer(&first), pointer(&second)}) == 1 && first == 12 && second == 456,
          "a later numeric matching failure preserves prior assignments and the unmatched destination");
    std::array<unsigned char, 6> guarded{};
    guarded.fill(0xaa);
    check(call("-2", "%ld", {pointer(guarded.data() + 1)}) == 1 &&
              guarded == std::array<unsigned char, 6>{0xaa, 0xfe, 0xff, 0xff, 0xff, 0xaa},
          "unaligned Windows long destinations receive exactly four bytes without overwriting guards");
    check(call("0z", "%i%c", {pointer(&first), pointer(&trailing)}) == 2 && first == 0 && trailing == 'z',
          "auto-base zero succeeds and leaves a nonnumeric suffix unread");
    std::array<char, 8> text_value{};
    check(call("ok:7", "%2s:%d", {pointer(text_value.data()), pointer(&first)}) == 2 &&
              std::string(text_value.data()) == "ok" && first == 7,
          "text and integer conversions share input position and Windows slot ordering");
    check(errno == E2BIG && crt_error() == 19 && get_last_error() == 87,
          "integer scanning uses locale-independent library parsing and preserves native and guest errors");
    errno = saved_errno;
}
