void native_text_conversion_checks() {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    Scope scope(runtime);
    using ToWide = std::int32_t(AMBROSIA_GUEST_ABI *)(std::uint32_t, std::uint32_t, const char *,
                                                      std::int32_t, void *, std::int32_t);
    using ToBytes = std::int32_t(AMBROSIA_GUEST_ABI *)(
        std::uint32_t, std::uint32_t, const void *, std::int32_t, void *, std::int32_t, const char *, void *);
    using GetCodepage = std::uint32_t(AMBROSIA_GUEST_ABI *)();
    const auto to_wide =
        reinterpret_cast<ToWide>(runtime.resolve(service_symbol("MultiByteToWideChar", "kernel32.dll")));
    const auto to_bytes =
        reinterpret_cast<ToBytes>(runtime.resolve(service_symbol("WideCharToMultiByte", "kernel32.dll")));
    const auto codepage =
        reinterpret_cast<GetCodepage>(runtime.resolve(service_symbol("GetACP", "kernel32.dll")));
    check(runtime.resolve(service_symbol("MultiByteToWideChar", "api-ms-win-core-string-l1-1-0.dll")) ==
                  reinterpret_cast<std::uintptr_t>(to_wide) &&
              runtime.resolve(service_symbol("WideCharToMultiByte", "kernelbase.dll")) ==
                  reinterpret_cast<std::uintptr_t>(to_bytes) &&
              codepage() == 65001,
          "native string contracts share typed UTF conversion and advertise the explicit UTF8 ANSI policy");
    constexpr char utf8[] = "A\xc3\xa9\xf0\x9f\x8c\xb4";
    constexpr char16_t utf16[] = u"A\u00e9\U0001f334";
    const auto saved_error = errno;
    errno = EDOM;
    crt_error() = 61;
    set_last_error(91);
    std::array<std::byte, 32> wide_output;
    wide_output.fill(std::byte{0xcc});
    const auto queried = to_wide(65001, 8, utf8, -1, nullptr, 0);
    const auto nonnull_query = to_wide(0, 0, utf8, -1, wide_output.data(), 0);
    const auto copied = to_wide(0, 8, utf8, -1, wide_output.data() + 3, 5);
    check(queried == 5 && nonnull_query == 5 && copied == 5 &&
              !std::memcmp(wide_output.data() + 3, utf16, sizeof(utf16)) &&
              wide_output[2] == std::byte{0xcc} && wide_output[13] == std::byte{0xcc} && errno == EDOM &&
              crt_error() == 61 && get_last_error() == 91,
          "UTF8 size queries and exact unaligned UTF16 output include the terminator for count-minus-one and "
          "preserve caller errors");
    wide_output.fill(std::byte{0xcc});
    const auto counted = to_wide(65001, 0, utf8, 7, wide_output.data() + 3, 10);
    check(counted == 4 && !std::memcmp(wide_output.data() + 3, utf16, 8) &&
              wide_output[11] == std::byte{0xcc},
          "explicit UTF8 counts convert precisely those bytes without ICU's convenience terminator changing "
          "adjacent output");
    const char embedded[] = {'A', '\0', 'B'};
    std::array<char16_t, 4> embedded_output{u'?', u'?', u'?', u'?'};
    const auto embedded_count = to_wide(65001, 0, embedded, 3, embedded_output.data(), 4);
    check(embedded_count == 3 && embedded_output[0] == u'A' && !embedded_output[1] &&
              embedded_output[2] == u'B' && embedded_output[3] == u'?',
          "explicit UTF8 input preserves embedded NUL units and subsequent characters rather than using a "
          "native string length");
    std::array<std::byte, 24> byte_output;
    byte_output.fill(std::byte{0xcc});
    const auto byte_query = to_bytes(65001, 128, utf16, -1, nullptr, 0, nullptr, nullptr);
    const auto encoded = to_bytes(0, 0, utf16, -1, byte_output.data() + 2, 8, nullptr, nullptr);
    check(byte_query == 8 && encoded == 8 && !std::memcmp(byte_output.data() + 2, utf8, sizeof(utf8)) &&
              byte_output[1] == std::byte{0xcc} && byte_output[10] == std::byte{0xcc},
          "the eight-argument reverse Windows ABI produces exact UTF8 bytes for BMP and supplementary UTF16 "
          "plus its requested NUL");
    byte_output.fill(std::byte{0xcc});
    const auto explicit_bytes = to_bytes(65001, 0, utf16, 4, byte_output.data() + 2, 20, nullptr, nullptr);
    check(explicit_bytes == 7 && !std::memcmp(byte_output.data() + 2, utf8, 7) &&
              byte_output[9] == std::byte{0xcc},
          "explicit UTF16 counts omit unrequested NUL bytes even when the output buffer has spare capacity");
    wide_output.fill(std::byte{0xcc});
    std::memcpy(wide_output.data() + 1, utf16, sizeof(utf16));
    const auto packed_source = to_bytes(65001, 128, wide_output.data() + 1, -1, nullptr, 0, nullptr, nullptr);
    check(packed_source == 8 && wide_output[0] == std::byte{0xcc} && wide_output[11] == std::byte{0xcc},
          "packed guest UTF16 input is copied into aligned native units without changing its surrounding "
          "fields");
    byte_output.fill(std::byte{0xcc});
    wide_output.fill(std::byte{0xcc});
    const auto short_wide = to_wide(65001, 0, utf8, -1, wide_output.data(), 4);
    const auto short_wide_error = get_last_error();
    const auto short_bytes = to_bytes(65001, 0, utf16, -1, byte_output.data(), 7, nullptr, nullptr);
    check(!short_wide && short_wide_error == 122 && !short_bytes && get_last_error() == 122 &&
              wide_output[0] == std::byte{0xcc} && byte_output[0] == std::byte{0xcc},
          "insufficient conversion outputs fail with the Windows buffer error before publishing any staged "
          "result");
    const char malformed[] = {static_cast<char>(0xc0), static_cast<char>(0xaf)};
    const auto strict_wide = to_wide(65001, 8, malformed, 2, wide_output.data(), 16);
    const auto strict_wide_error = get_last_error();
    const auto replaced_wide = to_wide(65001, 0, malformed, 2, wide_output.data(), 16);
    const char16_t replacements[] = {0xfffd, 0xfffd};
    check(!strict_wide && strict_wide_error == 1113 && replaced_wide == 2 &&
              !std::memcmp(wide_output.data(), replacements, sizeof(replacements)),
          "strict UTF8 rejects overlong encodings while normal conversion substitutes Unicode replacement "
          "characters");
    const char16_t malformed_wide[] = {0xd800, u'A'};
    const auto strict_bytes =
        to_bytes(65001, 128, malformed_wide, 2, byte_output.data(), 24, nullptr, nullptr);
    const auto strict_bytes_error = get_last_error();
    const auto replaced_bytes =
        to_bytes(65001, 0, malformed_wide, 2, byte_output.data(), 24, nullptr, nullptr);
    constexpr char replacement_utf8[] = "\xef\xbf\xbd"
                                        "A";
    check(!strict_bytes && strict_bytes_error == 1113 && replaced_bytes == 4 &&
              !std::memcmp(byte_output.data(), replacement_utf8, 4),
          "strict UTF16 rejects unpaired surrogates while normal reverse conversion uses U+FFFD without "
          "claiming a default codepage character");
    const auto wrong_mb_flags = to_wide(65001, 1, utf8, -1, nullptr, 0);
    const auto wrong_mb_error = get_last_error();
    const auto wrong_wc_flags = to_bytes(65001, 1024, utf16, -1, nullptr, 0, nullptr, nullptr);
    check(!wrong_mb_flags && wrong_mb_error == 1004 && !wrong_wc_flags && get_last_error() == 1004,
          "UTF8 only accepts the documented direction-specific invalid-character flag instead of legacy "
          "best-fit or normalization flags");
    const auto null_input = to_wide(65001, 0, nullptr, 1, nullptr, 0);
    const auto null_input_error = get_last_error();
    const auto zero_input = to_wide(65001, 0, utf8, 0, nullptr, 0);
    const auto zero_input_error = get_last_error();
    const auto negative_input = to_wide(65001, 0, utf8, -2, nullptr, 0);
    const auto negative_input_error = get_last_error();
    const auto missing_output = to_wide(65001, 0, utf8, 1, nullptr, 1);
    check(!null_input && null_input_error == 87 && !zero_input && zero_input_error == 87 && !negative_input &&
              negative_input_error == 87 && !missing_output && get_last_error() == 87,
          "null input, zero or invalid negative counts and missing nonempty output are rejected before "
          "source reads");
    std::uint32_t used = 99;
    const auto default_byte = to_bytes(65001, 0, utf16, -1, nullptr, 0, "?", nullptr);
    const auto default_error = get_last_error();
    const auto default_output = to_bytes(65001, 0, utf16, -1, nullptr, 0, nullptr, &used);
    const auto default_output_error = get_last_error();
    const auto same_pointer = to_wide(65001, 0, utf8, 1, const_cast<char *>(utf8), 1);
    check(!default_byte && default_error == 87 && !default_output && default_output_error == 87 &&
              used == 99 && !same_pointer && get_last_error() == 87,
          "UTF8 forbids default-character side channels and identical source/output pointers without writing "
          "to either");
    const auto too_large = to_wide(65001, 0, utf8, text_input_limit + 1, nullptr, 0);
    check(!too_large && get_last_error() == 8 && errno == EDOM && crt_error() == 61,
          "over-budget text counts fail before reading the small source rather than allocating unbounded "
          "conversion state");
    auto bytes = fixture();
    text(bytes, 0x440, "kernel32.dll");
    text(bytes, 0x4a2, "MultiByteToWideChar");
    const unsigned char code[] = {0x48, 0x83, 0xec, 0x38, 0xb9, 0xe9, 0xfd, 0,    0,    0x31, 0xd2,
                                  0x4c, 0x8d, 0x05, 0,    0,    0,    0,    0x41, 0xb9, 2,    0,
                                  0,    0,    0x48, 0x8d, 0x05, 0,    0,    0,    0,    0x48, 0x89,
                                  0x44, 0x24, 0x20, 0xc7, 0x44, 0x24, 0x28, 2,    0,    0,    0,
                                  0xff, 0x15, 0,    0,    0,    0,    0x48, 0x83, 0xc4, 0x38, 0xc3};
    std::memcpy(bytes.data() + 0x200, code, sizeof(code));
    put<std::uint32_t>(bytes, 0x20e, 0x21d0 - 0x1012);
    put<std::uint32_t>(bytes, 0x21b, 0x21f0 - 0x101f);
    put<std::uint32_t>(bytes, 0x22e, 0x2080 - 0x1032);
    text(bytes, 0x5d0, "OK");
    put<std::uint64_t>(bytes, 0x5f0, 0xccccccccccccccccull);
    auto image = pe::Image::load(bytes, [&](const auto &symbol) { return runtime.resolve(symbol); });
    const auto result = image.run_bootstrap();
    const char16_t expected[] = {u'O', u'K'};
    check(result == 2 && !std::memcmp(image.base() + 0x21f0, expected, sizeof(expected)) &&
              image.base()[0x21f4] == std::byte{0xcc},
          "original PE conversion passes four registers plus output/count stack slots through Microsoft ABI "
          "and preserves explicit-count output guards");
    errno = saved_error;
}
