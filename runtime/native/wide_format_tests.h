void wide_format_checks() {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    Scope scope(runtime);
    using Format = std::int32_t(AMBROSIA_GUEST_ABI *)(std::uint64_t, char16_t *, std::size_t,
                                                      const char16_t *, void *, const std::byte *);
    const auto address = runtime.resolve(service_symbol("__stdio_common_vswprintf_s", "ucrtbase.dll"));
    const auto format = reinterpret_cast<Format>(address);
    check(address && address == runtime.resolve(service_symbol("__stdio_common_vswprintf_s",
                                                               "api-ms-win-crt-stdio-l1-1-0.dll")),
          "secure wide formatting aliases share the typed six-argument Windows entrypoint");
    const auto call = [&](std::uint64_t options, char16_t *buffer, std::size_t size, const char16_t *pattern,
                          std::initializer_list<std::uint64_t> slots) {
        return format(options, buffer, size, pattern, nullptr,
                      reinterpret_cast<const std::byte *>(slots.begin()));
    };
    const auto saved_error = errno;
    errno = E2BIG;
    crt_error() = 19;
    set_last_error(87);
    std::array<char16_t, 128> output{};
    const auto guid_count =
        call(36, output.data(), 37, u"%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             {0x12345678, 0xabcd, 0x0123, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0x01, 0x23});
    check(guid_count == 36 && std::u16string(output.data()) == u"12345678-abcd-0123-4567-89abcdef0123",
          "the observed secure wide GUID workload consumes Windows slots and exactly37 UTF16 units including "
          "NUL");
    const auto integer_count = call(32, output.data(), output.size(), u"%ld/%I64d/%hu/%#x/%08.3d",
                                    {0xdeadbeeffffffffeull, 0xfffffffffffffffdull, 0x10001, 42, 7});
    check(integer_count == 21 && std::u16string(output.data()) == u"-2/-3/1/0x2a/     007",
          "wide output reuses Windows LLP64 integer and numeric field semantics");
    const auto numeric_count =
        call(32, output.data(), output.size(), u"%*.*f/%.0f/%%",
             {8, 2, std::bit_cast<std::uint64_t>(1.25), std::bit_cast<std::uint64_t>(2.5)});
    check(numeric_count == 12 && std::u16string(output.data()) == u"    1.25/2/%",
          "numeric wide conversions retain star-slot ordering and nearest-even rounding across fields");
    const auto legacy_strings =
        call(36, output.data(), output.size(), u"%s/%S/%ls/%hs",
             {reinterpret_cast<std::uintptr_t>(u"wide"), reinterpret_cast<std::uintptr_t>("narrow"),
              reinterpret_cast<std::uintptr_t>(u"explicit"), reinterpret_cast<std::uintptr_t>("byte")});
    check(legacy_strings == 25 && std::u16string(output.data()) == u"wide/narrow/explicit/byte",
          "legacy wide options select UTF16 lower-case strings and narrow upper-case strings with explicit "
          "length overrides");
    const auto standard_strings =
        call(32, output.data(), output.size(), u"%s/%S",
             {reinterpret_cast<std::uintptr_t>("byte"), reinterpret_cast<std::uintptr_t>(u"wide")});
    check(standard_strings == 9 && std::u16string(output.data()) == u"byte/wide",
          "standard wide options invert unqualified string selection while preserving UTF16 output");
    const auto unicode_count =
        call(36, output.data(), output.size(), u"Ω[%5.2s][%-4ls]",
             {reinterpret_cast<std::uintptr_t>(u"🙂tail"), reinterpret_cast<std::uintptr_t>(u"é")});
    check(unicode_count == 14 && std::u16string(output.data()) == u"Ω[   🙂][é   ]",
          "wide literals and strings retain UTF16 code units; precision/width count units instead of UTF8 "
          "bytes or display columns");
    const char16_t bounded[] = {u'a', u'b', u'c'};
    const auto bounded_count = call(36, output.data(), output.size(), u"[%*.*s][%.0s]",
                                    {0xfeed0000fffffffauLL, 3, reinterpret_cast<std::uintptr_t>(bounded), 1});
    check(bounded_count == 10 && std::u16string(output.data()) == u"[abc   ][]",
          "wide precision bounds unterminated strings and zero precision never reads the argument pointer");
    const auto character_count =
        call(36, output.data(), output.size(), u"%c%hc%lc%CZ", {0x03a9, 'a', 0x00e9, 0});
    check(character_count == 5 && output[0] == u'Ω' && output[1] == u'a' && output[2] == u'é' &&
              output[3] == 0 && output[4] == u'Z' && output[5] == 0,
          "wide character variants preserve UTF16 and embedded NUL while counting the full output");
    std::array<char16_t, 8> guarded;
    guarded.fill(u'R');
    const auto exact_fit = call(36, guarded.data() + 1, 4, u"abc", {});
    check(exact_fit == 3 && guarded[0] == u'R' && guarded[1] == u'a' && guarded[4] == 0 && guarded[5] == u'R',
          "secure wide buffer bounds are UTF16 units with space reserved for one terminator");
    const auto empty = call(36, guarded.data() + 1, 1, u"", {});
    check(empty == 0 && guarded[1] == 0 && guarded[0] == u'R' && guarded[2] == u'b',
          "empty secure output fits a one-unit destination without clearing adjacent content");
    check(errno == E2BIG && crt_error() == 19 && get_last_error() == 87,
          "successful secure wide output preserves native errno and guest error channels");

    auto caller = fixture();
    text(caller, 0x440, "ucrtbase.dll");
    text(caller, 0x4a2, "__stdio_common_vswprintf_s");
    const char16_t pattern[] = u"%d/%.1f";
    std::memcpy(caller.data() + 0x540, pattern, sizeof(pattern));
    put<std::uint64_t>(caller, 0x570, 42);
    put<std::uint64_t>(caller, 0x578, std::bit_cast<std::uint64_t>(2.5));
    constexpr unsigned char code[] = {
        0x48, 0x83, 0xec, 0x38, 0xb9, 32, 0,    0,    0,    0x48, 0x8d, 0x15, 0,   0,    0,    0,
        0x41, 0xb8, 16,   0,    0,    0,  0x4c, 0x8d, 0x0d, 0,    0,    0,    0,   0x48, 0xc7, 0x44,
        0x24, 0x20, 0,    0,    0,    0,  0x48, 0x8d, 0x05, 0,    0,    0,    0,   0x48, 0x89, 0x44,
        0x24, 0x28, 0xff, 0x15, 0,    0,  0,    0,    0x48, 0x83, 0xc4, 0x38, 0xc3};
    std::memcpy(caller.data() + 0x200, code, sizeof(code));
    put<std::uint32_t>(caller, 0x20c, 0x2110 - (0x1000 + 16));
    put<std::uint32_t>(caller, 0x219, 0x2140 - (0x1000 + 29));
    put<std::uint32_t>(caller, 0x229, 0x2170 - (0x1000 + 45));
    put<std::uint32_t>(caller, 0x234, 0x2080 - (0x1000 + 56));
    auto image = pe::Image::load(caller, [&](const auto &symbol) { return runtime.resolve(symbol); });
    const auto result = image.run_bootstrap();
    std::array<char16_t, 7> pe_output{};
    std::memcpy(pe_output.data(), image.base() + 0x2110, sizeof(pe_output));
    check(result == 6 && std::u16string(pe_output.data()) == u"42/2.5",
          "an original PE supplies UTF16 format/output and stack-passed locale/Windows va slots to the "
          "secure formatter");
    errno = saved_error;
}
