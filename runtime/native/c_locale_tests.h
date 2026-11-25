void c_locale_checks() {
    using namespace ambrosia::services;
    CrtLocale locale;
    bool categories = true;
    for (std::int32_t category = 0; category <= 5; ++category) {
        auto *name = locale.select(category, nullptr);
        categories = categories && name && std::string_view(name) == "C" &&
                     locale.select(category, "C") == name && locale.select(category, "") == name;
    }
    check(categories, "all guest locale categories query/select their stable C default");
    check(!locale.select(-1, "C") && !locale.select(6, "C") && !locale.select(INT32_MAX, nullptr),
          "guest locale categories reject values outside the Windows CRT range");
    auto *initial = locale.select(0, nullptr);
    check(!locale.select(0, "fr-FR") && !locale.select(0, "C.UTF-8") &&
              locale.select(0, nullptr) == initial && std::string_view(initial) == "C",
          "unavailable locale selection fails without changing the current C state");
    Runtime runtime;
    Runtime independent;
    using Select = char *(AMBROSIA_GUEST_ABI *)(std::int32_t, const char *);
    const auto select = reinterpret_cast<Select>(
        runtime.resolve(service_symbol("setlocale", "api-ms-win-crt-locale-l1-1-0.dll")));
    Scope scope(runtime);
    set_last_error(44);
    crt_error() = 13;
    runtime.lock_locales();
    auto *selected = select(0, "C");
    runtime.unlock_locales();
    check(selected && std::string_view(selected) == "C" && get_last_error() == 44 && crt_error() == 13,
          "Microsoft ABI locale selection shares recursive locking and preserves success errors");
    check(!select(0, "unavailable") && crt_error() == 22 && get_last_error() == 44 &&
              select(0, nullptr) == selected,
          "locale selection failure reports guest EINVAL and preserves previous locale");
    {
        Scope nested(independent);
        auto *other = select(0, nullptr);
        check(other && other != selected && std::string_view(other) == "C",
              "independent runtime owns its own locale result storage");
    }
    check(select(0, "") == selected, "default locale selection restores the original runtime storage");
    using Codepage = std::uint32_t(AMBROSIA_GUEST_ABI *)();
    using Length = std::int32_t(AMBROSIA_GUEST_ABI *)();
    using Names = char16_t **(AMBROSIA_GUEST_ABI *)();
    const auto codepage = reinterpret_cast<Codepage>(
        runtime.resolve(service_symbol("___lc_codepage_func", "api-ms-win-crt-locale-l1-1-0.dll")));
    const auto collation =
        reinterpret_cast<Codepage>(runtime.resolve(service_symbol("___lc_collate_cp_func", "ucrtbase.dll")));
    const auto length =
        reinterpret_cast<Length>(runtime.resolve(service_symbol("___mb_cur_max_func", "ucrtbase.dll")));
    const auto names =
        reinterpret_cast<Names>(runtime.resolve(service_symbol("___lc_locale_name_func", "ucrtbase.dll")));
    check(codepage() == 0 && collation() == 0 && length() == 1,
          "C locale CRT queries retain codepage-zero and single-byte invariants");
    auto **wide = names();
    check(wide && wide == names() && std::all_of(wide, wide + 6, [](auto *name) { return name == nullptr; }),
          "C locale owns a stable six-category array without NLS names");
    using Classification = const std::uint16_t *(AMBROSIA_GUEST_ABI *)();
    const auto classification = reinterpret_cast<Classification>(
        runtime.resolve(service_symbol("__pctype_func", "api-ms-win-crt-locale-l1-1-0.dll")));
    const auto *table = classification();
    check(table && table == classification() && table[-1] == 0,
          "C locale classification table retains a stable EOF slot before byte zero");
    check(table[0] == 0x20 && table['\t'] == 0x28 && table[' '] == 0x48 && table[127] == 0x20,
          "C classification distinguishes controls, whitespace and the blank-space flag");
    check(table['A'] == 0x181 && table['G'] == 0x101 && table['a'] == 0x182 && table['g'] == 0x102 &&
              table['0'] == 0x84 && table['!'] == 0x10,
          "C classification supplies Windows CRT letter, digit, hex and punctuation masks");
    const std::array<unsigned, 9> expected{26, 26, 10, 6, 32, 33, 1, 22, 52};
    std::array<unsigned, 9> actual{};
    for (unsigned character = 0; character < 256; ++character) {
        for (unsigned bit = 0; bit < actual.size(); ++bit) {
            actual[bit] += (table[character] & (1u << bit)) != 0;
        }
    }
    check(
        actual == expected && std::all_of(table + 128, table + 256, [](auto flags) { return flags == 0; }),
        "complete C byte table matches independent classification counts and leaves high bytes unclassified");
}
