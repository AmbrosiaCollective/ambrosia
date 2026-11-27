void user_locale_checks() {
    using namespace ambrosia;
    using namespace services;
    const auto invalid = [](UserLocale &profile, std::string_view tag) {
        try {
            profile.configure(tag);
        } catch (const std::invalid_argument &) {
            return true;
        }
        return false;
    };
    UserLocale defaults;
    check(defaults.name() == u"en-US" && runtime_rejected([&] { defaults.configure("fr-FR"); }),
          "the explicit English guest locale default freezes on its first read");
    UserLocale configured;
    configured.configure("sr-latn-rs");
    check(
        configured.name() == u"sr-Latn-RS" && runtime_rejected([&] { configured.configure("en-US"); }),
        "installed ICU canonicalizes complete BCP47 script/region names without consulting host preferences");
    UserLocale malformed;
    check(invalid(malformed, "") && invalid(malformed, "en_US") && invalid(malformed, "en--US") &&
              invalid(malformed, std::string("en\0US", 5)) && invalid(malformed, "\xc3\xa9") &&
              invalid(malformed, std::string(UserLocale::name_capacity, 'a')),
          "empty, partial, non-BCP47, embedded-NUL, non-ASCII and oversized locale inputs are rejected");
    malformed.configure("DE-de");
    check(malformed.name() == u"de-DE",
          "failed locale validation leaves the profile available for a valid explicit selection");
    Runtime runtime;
    Runtime independent;
    runtime.user_locale().configure("fr-ca");
    Scope scope(runtime);
    using Get = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, std::int32_t);
    const auto address = runtime.resolve(service_symbol("GetUserDefaultLocaleName", "kernel32.dll"));
    check(address &&
              address == runtime.resolve(service_symbol("GetUserDefaultLocaleName", "kernelbase.dll")) &&
              address == runtime.resolve(service_symbol("GetUserDefaultLocaleName",
                                                        "api-ms-win-core-localization-l1-2-0.dll")),
          "user locale aliases expose a typed two-argument Windows UTF16 API");
    if (!address) {
        return;
    }
    const auto get = reinterpret_cast<Get>(address);
    std::array<std::byte, 24> output{};
    output.fill(std::byte{0xcc});
    const auto saved_errno = errno;
    errno = E2BIG;
    crt_error() = 19;
    set_last_error(87);
    constexpr char16_t expected[] = u"fr-CA";
    check(get(output.data() + 1, 6) == 6 && !std::memcmp(output.data() + 1, expected, sizeof(expected)) &&
              output.front() == std::byte{0xcc} && output[sizeof(expected) + 1] == std::byte{0xcc} &&
              errno == E2BIG && crt_error() == 19 && get_last_error() == 87,
          "unaligned exact-capacity locale output counts its NUL, preserves guards and leaves errors intact");
    output.fill(std::byte{0xcc});
    check(get(output.data() + 1, 5) == 0 && get_last_error() == 122 &&
              std::all_of(output.begin(), output.end(), [](auto byte) { return byte == std::byte{0xcc}; }),
          "short locale buffers report insufficient buffer without partial writes");
    check(get(nullptr, 0) == 0 && get_last_error() == 122 && get(nullptr, 6) == 0 &&
              get_last_error() == 122 && get(output.data(), -1) == 0 && get_last_error() == 122 &&
              errno == E2BIG && crt_error() == 19,
          "null, zero and negative locale capacity fail rather than inventing a sizing-query result");
    check(runtime_rejected([&] { runtime.user_locale().configure("en-US"); }) &&
              std::string(runtime.locale().select(0, nullptr)) == "C",
          "reading the Win32 locale freezes its selection and leaves the CRT C locale unchanged");
    {
        Scope independent_scope(independent);
        std::array<char16_t, UserLocale::name_capacity> name{};
        check(get(name.data(), static_cast<std::int32_t>(name.size())) == 6 &&
                  std::u16string(name.data()) == u"en-US",
              "independent runtime scopes retain separate guest formatting preferences");
    }
    const auto english = lookup_locale_identifier("en-US");
    const auto neutral = lookup_locale_identifier("en");
    check(english && english->value == 0x0409 && !english->neutral && neutral && neutral->value == 0x0009 &&
              neutral->neutral && !lookup_locale_identifier("en-XX") && !lookup_locale_identifier("zz-ZZ"),
          "LCID lookup retains exact table mappings and rejects ICU's broader-language fallback");
    using Map = std::uint32_t(AMBROSIA_GUEST_ABI *)(const void *, std::uint32_t);
    const auto mapping_address = runtime.resolve(service_symbol("LocaleNameToLCID", "kernel32.dll"));
    check(mapping_address &&
              mapping_address == runtime.resolve(service_symbol("LocaleNameToLCID", "kernelbase.dll")) &&
              mapping_address == runtime.resolve(service_symbol("LocaleNameToLCID",
                                                                "api-ms-win-core-localization-l1-2-0.dll")),
          "locale identifier aliases share the typed Windows name-and-flags entrypoint");
    if (!mapping_address) {
        return;
    }
    const auto map = reinterpret_cast<Map>(mapping_address);
    errno = E2BIG;
    crt_error() = 19;
    set_last_error(87);
    check(map(u"en-US", 0) == 0x0409 && map(u"FR-ca", 0) == 0x0c0c && map(nullptr, 0) == 0x0c0c &&
              map(u"", 0) == 0x007f && errno == E2BIG && crt_error() == 19 && get_last_error() == 87,
          "specific, canonicalized, selected-user and invariant names resolve real identifiers without "
          "changing errors");
    check(map(u"en", 0x08000000) == 0x0009 && map(u"zh-Hant", 0x08000000) == 0x7c04 &&
              map(u"en-US", 0x08000000) == 0x0409,
          "the neutral-name flag permits known neutral identifiers without reducing specific names");
    std::array<std::byte, 16> packed{};
    packed.fill(std::byte{0xcc});
    constexpr char16_t german[] = u"de-DE";
    std::memcpy(packed.data() + 1, german, sizeof(german));
    check(map(packed.data() + 1, 0) == 0x0407 && packed.front() == std::byte{0xcc} &&
              packed[sizeof(german) + 1] == std::byte{0xcc},
          "packed UTF16 locale input maps without altering source storage or its guards");
    const std::array<char16_t, UserLocale::name_capacity> unterminated = [] {
        std::array<char16_t, UserLocale::name_capacity> value{};
        value.fill(u'a');
        return value;
    }();
    check(map(u"en-US", 1) == 0 && get_last_error() == 87 && map(u"en--US", 0) == 0 &&
              get_last_error() == 87 && map(u"é", 0) == 0 && get_last_error() == 87 &&
              map(unterminated.data(), 0) == 0 && get_last_error() == 87,
          "invalid flags, partial names, non-ASCII input and missing bounded NUL return parameter errors");
    auto bytes = fixture();
    text(bytes, 0x440, "kernel32.dll");
    text(bytes, 0x4a2, "GetUserDefaultLocaleName");
    constexpr unsigned char code[] = {0x48, 0x83, 0xec, 0x28, 0x48, 0x8d, 0x0d, 0, 0, 0, 0,
                                      0xba, 85,   0,    0,    0,    0xff, 0x15, 0, 0, 0, 0,
                                      0x48, 0x83, 0xc4, 0x28, 0x8b, 0x05, 0,    0, 0, 0, 0xc3};
    std::memcpy(bytes.data() + 0x200, code, sizeof(code));
    put<std::uint32_t>(bytes, 0x207, 0x2150 - 0x100b);
    put<std::uint32_t>(bytes, 0x212, 0x2080 - 0x1016);
    put<std::uint32_t>(bytes, 0x21c, 0x2150 - 0x1020);
    const auto root = std::filesystem::path("/work/user-locale-command");
    std::filesystem::create_directories(root);
    const auto image = root / "original.exe";
    {
        std::ofstream file(image, std::ios::binary);
        file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    std::ostringstream messages;
    std::ostringstream errors;
    const auto result =
        run_native_command(std::vector<std::filesystem::path>{"--run-bootstrap", image, "--package", root,
                                                              "--locale-name", "FR-ca"},
                           messages, errors);
    const auto first_letters = std::uint64_t('f') | (std::uint64_t('r') << 16);
    check(result == 0 && errors.str().empty() &&
              messages.str().find("\"bootstrap_result\":" + std::to_string(first_letters)) !=
                  std::string::npos,
          "an original PE receives the configured locale through actual CLI parsing, import and "
          "output-buffer ABI");
    auto identifier_bytes = bytes;
    text(identifier_bytes, 0x4a2, "LocaleNameToLCID");
    put<std::uint32_t>(identifier_bytes, 0x20c, 0);
    identifier_bytes[0x21a] = std::byte{0xc3}; // Return the API's LCID, without the name-load instruction.
    std::memcpy(identifier_bytes.data() + 0x550, german, sizeof(german));
    const auto identifier_image = root / "mapping.exe";
    {
        std::ofstream file(identifier_image, std::ios::binary);
        file.write(reinterpret_cast<const char *>(identifier_bytes.data()),
                   static_cast<std::streamsize>(identifier_bytes.size()));
    }
    check(run_native_command(std::vector<std::filesystem::path>{"--run-bootstrap", identifier_image,
                                                                "--package", root, "--locale-name", "fr-CA"},
                             messages, errors) == 0 &&
              errors.str().empty() && messages.str().find("\"bootstrap_result\":1031") != std::string::npos,
          "an original PE maps its explicit German input independently of the configured French user locale");
    check(run_native_command(std::vector<std::filesystem::path>{"--inspect", image, "--locale-name", "en-US"},
                             messages, errors) == 2 &&
              run_native_command(std::vector<std::filesystem::path>{"--run-bootstrap", image, "--locale-name",
                                                                    "en-US", "--locale-name", "fr-FR"},
                                 messages, errors) == 2,
          "guest locale selection requires execution mode and rejects duplicate configuration");
    check(run_native_command(std::vector<std::filesystem::path>{"--run-bootstrap", image, "--package", root,
                                                                "--locale-name", "en_US"},
                             messages, errors) == 2,
          "malformed CLI locale names are rejected before guest execution");
    errno = saved_errno;
}

int locale_rejection_probe(unsigned mode) {
    using namespace ambrosia::services;
    using Map = std::uint32_t(AMBROSIA_GUEST_ABI *)(const void *, std::uint32_t);
    Runtime runtime;
    Scope scope(runtime);
    const auto map =
        reinterpret_cast<Map>(runtime.resolve(service_symbol("LocaleNameToLCID", "kernel32.dll")));
    if (mode == 1) {
        (void)map(u"en-XX", 0);
    } else if (mode == 2) {
        (void)map(u"en", 0);
    } else if (mode == 3) {
        (void)map(u"!x-sys-default-locale", 0);
    } else if (mode == 4) {
        (void)map(u"de-DE_phoneb", 0);
    } else {
        return 2;
    }
    return 1;
}
