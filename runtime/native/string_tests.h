template <class Function> Function string_service(ambrosia::services::Runtime &runtime, const char *name) {
    const auto address = runtime.resolve(service_symbol(name, "api-ms-win-crt-string-l1-1-0.dll"));
    if (!address) {
        throw std::runtime_error(std::string("missing string service: ") + name);
    }
    return reinterpret_cast<Function>(address);
}

void string_service_checks() {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    Scope scope(runtime);
    bool aliases = true;
    for (const auto *name :
         {"strlen",    "wcslen",    "strcmp",    "strncmp", "wcscmp",  "wcscoll", "_stricmp",
          "_strnicmp", "_wcsicoll", "_wcsnicmp", "strcpy",  "strncpy", "wcsncpy", "strcat",
          "strncat",   "strcspn",   "strpbrk",   "isalnum", "isalpha", "isdigit", "islower",
          "isspace",   "isxdigit",  "tolower",   "toupper"}) {
        const auto address = runtime.resolve(service_symbol(name, "api-ms-win-crt-string-l1-1-0.dll"));
        aliases = aliases && address && address == runtime.resolve(service_symbol(name, "ucrtbase.dll")) &&
                  address == runtime.resolve(service_symbol(name, "msvcrt.dll"));
    }
    check(aliases, "stateless CRT string and classification aliases share their native facades");
    using Length = std::size_t(AMBROSIA_GUEST_ABI *)(const char *);
    using WideLength = std::size_t(AMBROSIA_GUEST_ABI *)(const char16_t *);
    using Compare = std::int32_t(AMBROSIA_GUEST_ABI *)(const char *, const char *);
    using CountedCompare = std::int32_t(AMBROSIA_GUEST_ABI *)(const char *, const char *, std::size_t);
    using WideCompare = std::int32_t(AMBROSIA_GUEST_ABI *)(const char16_t *, const char16_t *);
    using WideCountedCompare =
        std::int32_t(AMBROSIA_GUEST_ABI *)(const char16_t *, const char16_t *, std::size_t);
    using Copy = char *(AMBROSIA_GUEST_ABI *)(char *, const char *);
    using CountedCopy = char *(AMBROSIA_GUEST_ABI *)(char *, const char *, std::size_t);
    using WideCountedCopy = char16_t *(AMBROSIA_GUEST_ABI *)(char16_t *, const char16_t *, std::size_t);
    using Span = std::size_t(AMBROSIA_GUEST_ABI *)(const char *, const char *);
    using Find = char *(AMBROSIA_GUEST_ABI *)(const char *, const char *);
    using Classify = std::int32_t(AMBROSIA_GUEST_ABI *)(std::int32_t);
    const auto length = string_service<Length>(runtime, "strlen");
    const auto wide_length = string_service<WideLength>(runtime, "wcslen");
    const auto compare = string_service<Compare>(runtime, "strcmp");
    const auto bounded_compare = string_service<CountedCompare>(runtime, "strncmp");
    const auto wide_compare = string_service<WideCompare>(runtime, "wcscmp");
    const auto insensitive = string_service<Compare>(runtime, "_stricmp");
    const auto insensitive_counted = string_service<CountedCompare>(runtime, "_strnicmp");
    const auto wide_insensitive = string_service<WideCompare>(runtime, "_wcsicoll");
    const auto wide_counted = string_service<WideCountedCompare>(runtime, "_wcsnicmp");
    set_last_error(83);
    crt_error() = 17;
    const auto saved_errno = errno;
    errno = E2BIG;
    check(length("") == 0 && length("abc") == 3 && length("a\0b") == 1 && length("\xc3\xa9") == 2,
          "strlen counts bytes and stops at the first NUL without Unicode reinterpretation");
    check(wide_length(u"") == 0 && wide_length(u"A\U0001f334") == 3,
          "wcslen counts Windows UTF-16 code units rather than native wchar_t or Unicode scalars");
    const char high[] = {static_cast<char>(0x80), 0};
    check(compare("same", "same") == 0 && compare("a", "ab") < 0 && compare("z", "a") > 0 &&
              compare(high, "\x7f") > 0,
          "narrow comparison orders unsigned bytes and handles equality and prefixes");
    check(bounded_compare("abx", "aby", 2) == 0 && bounded_compare("abx", "aby", 3) < 0 &&
              bounded_compare("x", "y", 0) == 0,
          "counted comparison stops at its exact bound including zero");
    check(wide_compare(u"same", u"same") == 0 && wide_compare(u"a", u"ab") < 0 &&
              wide_compare(u"\uffff", u"\u7fff") > 0,
          "wide comparison uses unsigned UTF-16 units and stops at NUL");
    check(insensitive("File", "fILE") == 0 && insensitive("Z", "[") > 0 && insensitive("\xc0", "\xe0") < 0 &&
              insensitive_counted("aBx", "ABy", 2) == 0,
          "C-locale insensitive comparison folds ASCII and leaves high bytes unchanged");
    const auto collate = string_service<WideCompare>(runtime, "wcscoll");
    check(wide_insensitive(u"FILE", u"file") == 0 && wide_counted(u"aBx", u"ABy", 2) == 0 &&
              collate(u"Z", u"a") < 0 && wide_insensitive(u"\u00c4", u"\u00e4") < 0,
          "wide case and collation use the owned C-locale policy without host NLS tables");

    std::array<char, 16> buffer;
    buffer.fill('#');
    const auto copy = string_service<Copy>(runtime, "strcpy");
    const auto copy_counted = string_service<CountedCopy>(runtime, "strncpy");
    check(copy(buffer.data(), "cat") == buffer.data() && std::string_view(buffer.data()) == "cat" &&
              buffer[4] == '#',
          "strcpy copies its terminator and returns the original destination");
    buffer.fill('#');
    check(copy_counted(buffer.data(), "x", 4) == buffer.data() && buffer[0] == 'x' && buffer[1] == 0 &&
              buffer[2] == 0 && buffer[3] == 0 && buffer[4] == '#',
          "strncpy pads exactly the requested extent when the source is shorter");
    buffer.fill('#');
    copy_counted(buffer.data(), "long", 3);
    check(buffer[0] == 'l' && buffer[1] == 'o' && buffer[2] == 'n' && buffer[3] == '#',
          "bounded copy does not invent a terminator after a full truncated extent");
    copy_counted(buffer.data(), "different", 0);
    check(buffer[0] == 'l' && buffer[3] == '#', "zero-count copy does not write destination bytes");
    std::array<char16_t, 8> wide_buffer;
    wide_buffer.fill(u'#');
    const auto wide_copy = string_service<WideCountedCopy>(runtime, "wcsncpy");
    check(wide_copy(wide_buffer.data(), u"A\U0001f334", 5) == wide_buffer.data() && wide_buffer[0] == u'A' &&
              wide_buffer[1] == 0xd83c && wide_buffer[2] == 0xdf34 && wide_buffer[3] == 0 &&
              wide_buffer[4] == 0 && wide_buffer[5] == u'#',
          "wide bounded copy preserves surrogate units and pads the requested number of 16-bit elements");
    wide_buffer.fill(u'#');
    wide_copy(wide_buffer.data(), u"\U0001f334", 1);
    check(wide_buffer[0] == 0xd83c && wide_buffer[1] == u'#',
          "UTF-16 bounded copy counts code units even when its bound splits a surrogate pair");
    const auto append = string_service<Copy>(runtime, "strcat");
    const auto append_counted = string_service<CountedCopy>(runtime, "strncat");
    buffer.fill('#');
    copy(buffer.data(), "hi");
    check(append(buffer.data(), "!") == buffer.data() && std::string_view(buffer.data()) == "hi!",
          "strcat appends through the source terminator and returns the original destination");
    check(append_counted(buffer.data(), "abcd", 2) == buffer.data() &&
              std::string_view(buffer.data()) == "hi!ab" && buffer[6] == '#',
          "strncat always terminates its bounded append without padding beyond it");
    append_counted(buffer.data(), "ignored", 0);
    check(std::string_view(buffer.data()) == "hi!ab", "zero-count append retains the existing string");
    const auto span = string_service<Span>(runtime, "strcspn");
    const auto find = string_service<Find>(runtime, "strpbrk");
    const char search_text[] = "abc,def";
    check(span(search_text, ",;") == 3 && span("abc", "") == 3 && span(",abc", ",") == 0,
          "strcspn returns the unmatched prefix length including empty-set and initial-match cases");
    check(find(search_text, ";,") == search_text + 3 && !find(search_text, "xyz") && !find(search_text, ""),
          "strpbrk returns a borrowed pointer into the original string or null");

    struct ClassificationCase {
        const char *name;
        std::int32_t member;
        std::int32_t nonmember;
    };

    for (const auto &test : {ClassificationCase{"isalnum", '9', '_'},
                             {"isalpha", 'A', '9'},
                             {"isdigit", '9', 'A'},
                             {"islower", 'z', 'Z'},
                             {"isspace", '\t', 'x'},
                             {"isxdigit", 'f', 'g'}}) {
        const auto classify = string_service<Classify>(runtime, test.name);
        check(classify(test.member) != 0 && classify(test.nonmember) == 0 && classify(-1) == 0 &&
                  classify(255) == 0,
              "C classification uses the owned byte table and handles EOF and high bytes without host locale "
              "state");
    }
    const auto lower = string_service<Classify>(runtime, "tolower");
    const auto upper = string_service<Classify>(runtime, "toupper");
    check(lower('Z') == 'z' && lower('z') == 'z' && upper('a') == 'A' && upper('A') == 'A' &&
              lower(-1) == -1 && upper(-1) == -1 && lower(255) == 255 && upper(255) == 255,
          "C case conversion folds ASCII and preserves EOF and non-ASCII byte values");
    check(crt_error() == 17 && get_last_error() == 83 && errno == E2BIG,
          "stateless CRT string operations preserve guest CRT errno, Win32 LastError and native errno");
    auto caller = fixture();
    text(caller, 0x440, "ucrtbase.dll");
    text(caller, 0x4a2, "strlen");
    text(caller, 0x4c0, "sample");
    constexpr unsigned char code[] = {0x48, 0x83, 0xec, 0x28, 0x48, 0x8d, 0x0d, 0,    0,    0,    0,
                                      0xff, 0x15, 0,    0,    0,    0,    0x48, 0x83, 0xc4, 0x28, 0xc3};
    std::memcpy(caller.data() + 0x200, code, sizeof(code));
    put<std::uint32_t>(caller, 0x207, 0x20c0 - (0x1000 + 11));
    put<std::uint32_t>(caller, 0x20d, 0x2080 - (0x1000 + 17));
    auto image = pe::Image::load(caller, [&](const auto &symbol) { return runtime.resolve(symbol); });
    check(image.run_bootstrap() == 6,
          "an original PE import receives the actual string length through the Microsoft ABI");
    errno = saved_errno;
}
