void readonly_file_checks() {
    using namespace ambrosia;
    using namespace services;
    const auto root = std::filesystem::path("/work/readonly-files");
    const auto package = root / "assets";
    const auto image = root / "bin" / "app.exe";
    std::filesystem::create_directories(package / "Folder");
    std::filesystem::create_directories(image.parent_path());
    const auto write = [](const auto &path, const std::string &contents) {
        std::ofstream stream(path, std::ios::binary);
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!stream) {
            throw std::runtime_error("cannot create original metadata fixture");
        }
    };
    write(package / "Folder" / "Item.dat", "fixture");
    write(package / std::filesystem::path(u"unicode-\u00e9.dat"), "unicode");
    write(package / "Case.dat", "upper");
    write(package / "case.DAT", "lower");
    write(root / "outside.dat", "not exposed");
    write(image, "original fixture only");
    std::filesystem::create_symlink(root / "outside.dat", package / "outside-link");
    std::filesystem::create_symlink("Folder/Item.dat", package / "inside-link");
    Runtime runtime;
    Runtime independent;
    Scope scope(runtime);
    runtime.files().bind(package, image);
    using AttributesWide = std::uint32_t(AMBROSIA_GUEST_ABI *)(const void *);
    using AttributesBytes = std::uint32_t(AMBROSIA_GUEST_ABI *)(const char *);
    const auto wide = reinterpret_cast<AttributesWide>(
        runtime.resolve(service_symbol("GetFileAttributesW", "kernel32.dll")));
    const auto bytes = reinterpret_cast<AttributesBytes>(
        runtime.resolve(service_symbol("GetFileAttributesA", "kernel32.dll")));
    check(runtime.resolve(service_symbol("GetFileAttributesW", "api-ms-win-core-file-l1-1-0.dll")) ==
                  reinterpret_cast<std::uintptr_t>(wide) &&
              runtime.resolve(service_symbol("GetFileAttributesA", "kernelbase.dll")) ==
                  reinterpret_cast<std::uintptr_t>(bytes),
          "file-attribute contracts share actual dynamically rooted read-only metadata services");
    const auto saved_error = errno;
    errno = EDOM;
    crt_error() = 61;
    set_last_error(91);
    const auto file = wide(u"Q:\\folder\\ITEM.dat");
    const auto directory = wide(u"Q:\\Folder\\");
    const auto relative = bytes("Folder/Item.dat");
    const auto volume = wide(u"assets:\\Folder\\.\\Item.dat");
    check(file == 1 && directory == 17 && relative == file && volume == file && errno == EDOM &&
              crt_error() == 61 && get_last_error() == 91,
          "DOS/named volume hints, separators and relative paths resolve real read-only files/directories "
          "with ASCII case fallback and intact errors");
    const auto resolved = runtime.files().resolve("Folder/Item.dat");
    const auto metadata = resolved.directory->inspect(resolved.components);
    check(!resolved.error && metadata.error == host::FileLookupError::none && !metadata.metadata.directory &&
              metadata.metadata.bytes == 7 && independent.files().attributes("Folder/Item.dat").error == 1,
          "shared file resolution retains actual seven-byte metadata without configuring or exposing an "
          "independent runtime");
    const auto native = (package / "Folder" / "Item.dat").generic_string();
    auto backslashed = native;
    std::replace(backslashed.begin(), backslashed.end(), '/', '\\');
    const auto native_file = bytes(native.c_str());
    const auto native_backslash = bytes(backslashed.c_str());
    const auto native_image = wide(image.u16string().c_str());
    check(native_file == 1 && native_backslash == 1 && native_image == 1,
          "native package/image origins route through configured roots even when the guest changes their "
          "separators");
    std::array<std::byte, 64> packed;
    packed.fill(std::byte{0xcc});
    constexpr char16_t unicode_path[] = u"Q:/unicode-\u00e9.dat";
    std::memcpy(packed.data() + 1, unicode_path, sizeof(unicode_path));
    check(wide(packed.data() + 1) == 1 && bytes("Q:/unicode-\xc3\xa9.dat") == 1 &&
              packed[0] == std::byte{0xcc} && packed[sizeof(unicode_path) + 1] == std::byte{0xcc},
          "UTF16 packed inputs and explicit UTF8 ANSI paths query the same exact Unicode filename without "
          "modifying their storage");
    const auto missing = wide(u"Q:/Folder/missing.dat");
    const auto missing_error = get_last_error();
    const auto missing_parent = wide(u"Q:/absent/item.dat");
    const auto parent_error = get_last_error();
    const auto not_directory = wide(u"Q:/Folder/Item.dat/child");
    const auto not_directory_error = get_last_error();
    const auto trailing_file = wide(u"Q:/Folder/Item.dat/");
    check(missing == 0xffffffffu && missing_error == 2 && missing_parent == 0xffffffffu &&
              parent_error == 3 && not_directory == 0xffffffffu && not_directory_error == 3 &&
              trailing_file == 0xffffffffu && get_last_error() == 3,
          "metadata errors distinguish an absent file, missing/non-directory parent and directory-only "
          "suffix without fabricated existence");
    const auto outside_name = (root / "outside.dat").generic_string();
    const auto outside = bytes(outside_name.c_str());
    const auto outside_error = get_last_error();
    const auto traversal = wide(u"Q:/../outside.dat");
    const auto traversal_error = get_last_error();
    const auto network = wide(u"\\\\server\\share\\item");
    const auto network_error = get_last_error();
    const auto uri = wide(u"https://server/item");
    check(outside == 0xffffffffu && outside_error == 5 && traversal == 0xffffffffu && traversal_error == 5 &&
              network == 0xffffffffu && network_error == 5 && uri == 0xffffffffu && get_last_error() == 5,
          "native outside-root paths, parent escapes, UNC and URI requests cannot cross the declared asset "
          "namespace");
    const auto external_link = wide(u"Q:/outside-link");
    const auto external_error = get_last_error();
    const auto internal_link = wide(u"Q:/inside-link");
    check(external_link == 0xffffffffu && external_error == 5 && internal_link == 0xffffffffu &&
              get_last_error() == 5,
          "descriptor lookups reject both escaping and internal symlink traversal instead of silently "
          "exposing link targets");
    const auto root_metadata = wide(u"Q:/Folder/..");
    const auto dot = wide(u".");
    const auto dot_file = wide(u"Q:/Folder/Item.dat/.");
    check(root_metadata == 17 && dot == 17 && dot_file == 0xffffffffu && get_last_error() == 3,
          "dot normalization retains the virtual root and directory requirements without allowing a regular "
          "file to masquerade as a directory");
    const auto exact_case = wide(u"Q:/Case.dat");
    const auto ambiguous = wide(u"Q:/CASE.DAT");
    check(exact_case == 1 && ambiguous == 0xffffffffu && get_last_error() == 123,
          "exact native spelling wins while ambiguous ASCII case aliases fail rather than selecting an "
          "arbitrary file");
    const auto null_path = wide(nullptr);
    const auto null_error = get_last_error();
    const auto empty = wide(u"");
    const auto empty_error = get_last_error();
    const auto drive_relative = wide(u"Q:Folder/Item.dat");
    const auto relative_error = get_last_error();
    const char malformed[] = {static_cast<char>(0xff), '\0'};
    const auto bad_utf8 = bytes(malformed);
    const auto bad_utf8_error = get_last_error();
    const char16_t malformed_wide[] = {0xd800, 0};
    const auto bad_utf16 = wide(malformed_wide);
    check(null_path == 0xffffffffu && null_error == 87 && empty == 0xffffffffu && empty_error == 123 &&
              drive_relative == 0xffffffffu && relative_error == 123 && bad_utf8 == 0xffffffffu &&
              bad_utf8_error == 123 && bad_utf16 == 0xffffffffu && get_last_error() == 123 && errno == EDOM &&
              crt_error() == 61,
          "missing/empty/drive-relative and malformed-Unicode paths fail explicitly while preserving native "
          "and CRT errors");
    const auto invalid_component = resolved.directory->inspect(std::array<std::string, 1>{".."});
    check(invalid_component.error == host::FileLookupError::invalid,
          "the host descriptor boundary independently refuses traversal even when invoked without the "
          "portable frontend");
    const auto descriptor_count = [] {
        std::size_t count = 0;
        for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd")) {
            (void)entry;
            ++count;
        }
        return count;
    };
    const auto before = descriptor_count();
    bool repeated = true;
    for (unsigned index = 0; index < 100; ++index) {
        const auto queried = wide(u"Q:/folder/ITEM.dat");
        repeated = repeated && queried == 1;
    }
    {
        ReadonlyFiles temporary;
        temporary.bind(package, image);
        repeated = repeated && temporary.attributes("Folder/Item.dat").value == 1;
    }
    const auto after = descriptor_count();
    check(repeated && before == after, "repeated case-folded queries and temporary root ownership release "
                                       "descriptors and keep readdir state independent");
    auto fixture_bytes = fixture();
    text(fixture_bytes, 0x440, "kernel32.dll");
    text(fixture_bytes, 0x4a2, "GetFileAttributesW");
    const unsigned char code[] = {0x48, 0x83, 0xec, 0x28, 0x48, 0x8d, 0x0d, 0,    0,    0,    0,
                                  0xff, 0x15, 0,    0,    0,    0,    0x48, 0x83, 0xc4, 0x28, 0xc3};
    std::memcpy(fixture_bytes.data() + 0x200, code, sizeof(code));
    put<std::uint32_t>(fixture_bytes, 0x207, 0x21b0 - 0x100b);
    put<std::uint32_t>(fixture_bytes, 0x20d, 0x2080 - 0x1011);
    constexpr char16_t guest_path[] = u"Q:/Folder/Item.dat";
    std::memcpy(fixture_bytes.data() + 0x5b0, guest_path, sizeof(guest_path));
    auto fixture_image =
        pe::Image::load(fixture_bytes, [&](const auto &symbol) { return runtime.resolve(symbol); });
    check(fixture_image.run_bootstrap() == 1, "an original PE import queries actual confined file metadata "
                                              "through Microsoft ABI and its UTF16 filename");
    errno = saved_error;
}
