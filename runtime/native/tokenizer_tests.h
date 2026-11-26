struct TokenizerInvalidRecord {
    unsigned calls = 0;
    bool valid_context = true;
    bool empty_metadata = true;
    bool invalid_error = true;
};

TokenizerInvalidRecord *tokenizer_invalid_record = nullptr;

void AMBROSIA_GUEST_ABI tokenizer_invalid_handler(const char16_t *expression, const char16_t *function,
                                                  const char16_t *file, std::uint32_t line,
                                                  std::uintptr_t reserved) {
    auto &record = *tokenizer_invalid_record;
    ++record.calls;
    record.valid_context = record.valid_context && ambrosia::guest::ThreadContext::current();
    record.empty_metadata = record.empty_metadata && !expression && !function && !file && !line && !reserved;
    record.invalid_error = record.invalid_error && ambrosia::services::crt_error() == 22;
}

void secure_tokenizer_checks() {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    Scope scope(runtime);
    guest::ThreadContext thread;
    auto activation = thread.activate();
    using Tokenize = char *(AMBROSIA_GUEST_ABI *)(char *, const char *, char **);
    const auto address = runtime.resolve(service_symbol("strtok_s", "ucrtbase.dll"));
    const auto tokenize = reinterpret_cast<Tokenize>(address);
    check(address && runtime.resolve(service_symbol("strtok_s", "msvcrt.dll")) == address &&
              runtime.resolve(service_symbol("strtok_s", "api-ms-win-crt-string-l1-1-0.dll")) == address,
          "secure tokenizer aliases reach one Windows three-argument byte-string service");
    const auto saved_error = errno;
    errno = EDOM;
    crt_error() = 61;
    set_last_error(91);
    char input_text[] = ",,alpha;beta,,gamma;;";
    char *context = nullptr;
    const auto first = tokenize(input_text, ",;", &context);
    const auto second = tokenize(nullptr, ",;", &context);
    const auto third = tokenize(nullptr, ",;", &context);
    const auto end = tokenize(nullptr, ",;", &context);
    const auto repeated = tokenize(nullptr, ",;", &context);
    check(first == input_text + 2 && !std::strcmp(first, "alpha") && second && !std::strcmp(second, "beta") &&
              third && !std::strcmp(third, "gamma") && !end && !repeated &&
              context == input_text + sizeof(input_text) - 1 && input_text[0] == ',' &&
              input_text[1] == ',' && errno == EDOM && crt_error() == 61 && get_last_error() == 91,
          "secure tokenization skips runs of separators, terminates actual tokens and preserves stable "
          "exhaustion plus caller error state");
    char left[] = "alpha/beta";
    char right[] = "one:two";
    char *left_context = nullptr, *right_context = nullptr;
    const auto left_first = tokenize(left, "/", &left_context);
    const auto right_first = tokenize(right, ":", &right_context);
    const auto left_second = tokenize(nullptr, "/", &left_context);
    const auto right_second = tokenize(nullptr, ":", &right_context);
    check(!std::strcmp(left_first, "alpha") && !std::strcmp(right_first, "one") &&
              !std::strcmp(left_second, "beta") && !std::strcmp(right_second, "two"),
          "interleaved callers own independent continuation pointers without hidden CRT or native tokenizer "
          "state");
    char changing[] = "one,two;three/four";
    context = nullptr;
    const auto a = tokenize(changing, ",", &context);
    const auto b = tokenize(nullptr, ";", &context);
    const auto c = tokenize(nullptr, "/", &context);
    const auto d = tokenize(nullptr, "", &context);
    check(!std::strcmp(a, "one") && !std::strcmp(b, "two") && !std::strcmp(c, "three") &&
              !std::strcmp(d, "four"),
          "delimiter sets are read anew for each continuation rather than captured from the first call");
    char whole[] = " a b ";
    char empty[] = "";
    char separators[] = ";;;";
    context = nullptr;
    const auto whole_result = tokenize(whole, "", &context);
    const auto empty_result = tokenize(empty, ",", &context);
    const auto separators_result = tokenize(separators, ";", &context);
    check(whole_result == whole && !std::strcmp(whole_result, " a b ") && !empty_result &&
              !separators_result && context == separators + 3 && crt_error() == 61 && get_last_error() == 91,
          "empty delimiters keep the whole token while empty and all-delimiter inputs end normally without "
          "invalid-parameter errors");
    char high[] = {'a', static_cast<char>(0xff), 'b', '\0'};
    const char high_delimiter[] = {static_cast<char>(0xff), '\0'};
    context = nullptr;
    const auto high_first = tokenize(high, high_delimiter, &context);
    const auto high_second = tokenize(nullptr, high_delimiter, &context);
    check(!std::strcmp(high_first, "a") && !std::strcmp(high_second, "b"),
          "tokenizer delimiters compare byte values above127 without signed-character classification or "
          "locale rules");
    std::array<std::byte, 16> context_guard;
    context_guard.fill(std::byte{0xcc});
    char packed[] = "x/y";
    auto **packed_context = reinterpret_cast<char **>(context_guard.data() + 3);
    const auto packed_first = tokenize(packed, "/", packed_context);
    const auto packed_second = tokenize(nullptr, "/", packed_context);
    char *packed_end = nullptr;
    std::memcpy(&packed_end, context_guard.data() + 3, sizeof(packed_end));
    check(packed_first == packed && packed_second == packed + 2 && packed_end == packed + 3 &&
              context_guard[2] == std::byte{0xcc} && context_guard[11] == std::byte{0xcc},
          "continuation pointers from packed guest storage use exactly eight unaligned bytes without "
          "overwriting neighboring fields");
    TokenizerInvalidRecord record;
    tokenizer_invalid_record = &record;
    const auto previous = runtime.invalid_parameters().replace(&tokenizer_invalid_handler);
    char untouched[] = "a,b";
    context = untouched + 1;
    const auto no_delimiters = tokenize(untouched, nullptr, &context);
    const auto first_error = crt_error();
    const auto no_context = tokenize(untouched, ",", nullptr);
    char *missing = nullptr;
    const auto no_continuation = tokenize(nullptr, ",", &missing);
    check(!previous && !no_delimiters && first_error == 22 && !no_context && !no_continuation &&
              record.calls == 3 && record.valid_context && record.empty_metadata && record.invalid_error &&
              !std::strcmp(untouched, "a,b") && context == untouched + 1 && !missing && crt_error() == 22 &&
              get_last_error() == 91 && errno == EDOM,
          "invalid delimiter/context/continuation arguments set guest EINVAL before the real handler and "
          "preserve input, continuation and native errors");
    (void)runtime.invalid_parameters().replace(previous);
    tokenizer_invalid_record = nullptr;
    auto bytes = fixture();
    text(bytes, 0x440, "ucrtbase.dll");
    text(bytes, 0x4a2, "strtok_s");
    const unsigned char code[] = {0x48, 0x83, 0xec, 0x28, 0x48, 0x8d, 0x0d, 0,    0,    0,    0,    0x48,
                                  0x8d, 0x15, 0,    0,    0,    0,    0x4c, 0x8d, 0x05, 0,    0,    0,
                                  0,    0xff, 0x15, 0,    0,    0,    0,    0x48, 0x83, 0xc4, 0x28, 0xc3};
    std::memcpy(bytes.data() + 0x200, code, sizeof(code));
    put<std::uint32_t>(bytes, 0x207, 0x21d0 - 0x100b);
    put<std::uint32_t>(bytes, 0x20e, 0x21e0 - 0x1012);
    put<std::uint32_t>(bytes, 0x215, 0x21f0 - 0x1019);
    put<std::uint32_t>(bytes, 0x21b, 0x2080 - 0x101f);
    text(bytes, 0x5d0, "one,two");
    text(bytes, 0x5e0, ",");
    put<std::uint64_t>(bytes, 0x5f0, 0);
    auto image = pe::Image::load(bytes, [&](const auto &symbol) { return runtime.resolve(symbol); });
    const auto result = image.run_bootstrap();
    char *guest_context = nullptr;
    std::memcpy(&guest_context, image.base() + 0x21f0, sizeof(guest_context));
    check(result == reinterpret_cast<std::uintptr_t>(image.base() + 0x21d0) &&
              !std::strcmp(reinterpret_cast<const char *>(result), "one") &&
              guest_context == reinterpret_cast<const char *>(image.base() + 0x21d4),
          "original PE entry passes real string/delimiter/context pointers through Microsoft ABI and "
          "receives its in-place token address");
    errno = saved_error;
}
