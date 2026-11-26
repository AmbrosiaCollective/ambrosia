void package_identity_checks() {
    using namespace ambrosia;
    using namespace services;
    const auto parse = [](std::string_view text, PackageDescriptor kind = PackageDescriptor::game_config) {
        return parse_package_identity({reinterpret_cast<const std::byte *>(text.data()), text.size()}, kind);
    };
    const auto rejected = [&](std::string_view text,
                              PackageDescriptor kind = PackageDescriptor::game_config) {
        try {
            (void)parse(text, kind);
            return false;
        } catch (const std::runtime_error &) {
            return true;
        }
    };
    const std::string identity = "<Identity Name='Ambrosia.Example' Publisher='CN=Example &amp; Co' "
                                 "Version='1.2.3.4'/>";
    const auto game = parse("<?xml version='1.0'?><Game>" + identity + "</Game>");
    const auto manifest =
        parse("<Package xmlns='urn:example'>" + identity + "</Package>", PackageDescriptor::appx_manifest);
    check(game == manifest && game.name == "Ambrosia.Example" && game.publisher == "CN=Example & Co" &&
              game.version == std::array<std::uint16_t, 4>{1, 2, 3, 4},
          "original game/appx descriptor forms share a validated identity, decoded attributes and exact "
          "four-component version without platform path assumptions");
    const std::u16string wide = u"\ufeff<Game><Identity Name='Ambrosia.Utf16' Publisher='CN=\u00e9' "
                                "Version='1.2.3.4'/></Game>";
    const auto utf16 = parse_package_identity(std::as_bytes(std::span(wide.data(), wide.size())),
                                              PackageDescriptor::game_config);
    check(utf16.name == "Ambrosia.Utf16" && utf16.publisher == "CN=\xc3\xa9",
          "Expat decodes bounded UTF16 metadata to the same UTF8 identity model");
    check(
        rejected("<Other>" + identity + "</Other>") && rejected("<Game>" + identity) &&
            rejected("<Game>" + identity + identity + "</Game>") &&
            rejected("<Game><Wrapper>" + identity + "</Wrapper></Game>"),
        "wrong roots, malformed XML, duplicate and nested substitute identities cannot configure a package");
    check(
        rejected("<Game><Identity Name='Example' Version='1.2.3.4'/></Game>") &&
            rejected("<Game><Identity Name='Example' Publisher='CN=Example' Version='1.2.3'/></Game>") &&
            rejected(
                "<Game><Identity Name='Example' Publisher='CN=Example' Version='1.2.65536.4'/></Game>") &&
            rejected("<Game><Identity Name='Example' Publisher='CN=Example' Version='1.2.3.4.5'/></Game>") &&
            rejected("<Game><Identity Name='Bad/Name' Publisher='CN=Example' Version='1.2.3.4'/></Game>"),
        "missing fields, malformed/overflowing versions and invalid identifier characters fail explicitly");
    const auto long_name = std::string(package_identifier_limit + 1, 'A');
    check(rejected("<Game><Identity Name='" + long_name +
                   "' Publisher='CN=Example' Version='1.2.3.4'/></Game>") &&
              rejected("<Game><Identity Name='Example' Publisher='" + std::string(257, 'A') +
                       "' Version='1.2.3.4'/></Game>"),
          "long identifiers or publishers fail without truncating distinct package identities together");
    check(rejected("<!DOCTYPE Game [<!ENTITY injected 'Example'>]><Game>" + identity + "</Game>") &&
              rejected("<!DOCTYPE Game SYSTEM 'file:///work/not-a-descriptor'><Game>" + identity + "</Game>"),
          "internal/external DTD declarations stop parsing before any entity resolution or external I/O");
    std::string deep = "<Game>" + identity;
    for (unsigned index = 0; index < 32; ++index) {
        deep += "<x>";
    }
    for (unsigned index = 0; index < 32; ++index) {
        deep += "</x>";
    }
    deep += "</Game>";
    std::string many = "<Game>" + identity;
    for (unsigned index = 0; index < 2048; ++index) {
        many += "<x/>";
    }
    many += "</Game>";
    check(
        rejected(deep) && rejected(many) && rejected("") &&
            rejected(std::string(package_descriptor_limit + 1, ' ')),
        "depth, element and byte limits reject over-budget descriptors even after a valid identity appears");
}

void native_package_context_checks() {
    using namespace ambrosia;
    using namespace services;
    const auto write = [](const auto &path, const std::string &text) {
        std::ofstream stream(path, std::ios::binary);
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!stream) {
            throw std::runtime_error("cannot write original package descriptor fixture");
        }
    };
    const auto descriptor = [](const char *root, const char *publisher, const char *version) {
        return std::string("<") + root + "><Identity Name='Ambrosia.Example' Publisher='" + publisher +
               "' Version='" + version + "'/></" + root + ">";
    };
    const auto root = std::filesystem::path("/work/package-context");
    const auto package = root / "assets";
    const auto image = root / "bin" / "app.exe";
    std::filesystem::create_directories(package);
    std::filesystem::create_directories(image.parent_path());
    write(image, "original fixture");
    write(package / "MicrosoftGame.Config", descriptor("Game", "CN=Example", "1.2.3.4"));
    write(package / "AppxManifest.xml", descriptor("Package", "CN=Example", "1.2.3.4"));
    Runtime runtime;
    Runtime independent;
    Scope scope(runtime);
    runtime.files().bind(package, image);
    runtime.gdk_packages().bind(runtime.files());
    (void)runtime.gdk_bootstrap().initialize(1, 2);
    using Query = std::int32_t(AMBROSIA_GUEST_ABI *)(const void *, const void *, void *);
    const auto query =
        reinterpret_cast<Query>(runtime.resolve(service_symbol("QueryApiImpl", "xgameruntime.dll")));
    void *object = nullptr;
    const auto queried = query(&package_class_id, &package_interface_id, &object);
    const auto *table = *static_cast<const std::uintptr_t *const *>(object);
    using Identifier = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, std::size_t, char *);
    using Packaged = bool(AMBROSIA_GUEST_ABI *)(void *);
    using InterfaceQuery = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, const void *, void *);
    using Release = std::uint32_t(AMBROSIA_GUEST_ABI *)(void *);
    const auto identifier = reinterpret_cast<Identifier>(table[3]);
    const auto packaged = reinterpret_cast<Packaged>(table[4]);
    const auto qi = reinterpret_cast<InterfaceQuery>(table[0]);
    const auto release = reinterpret_cast<Release>(table[2]);
    check(queried == 0 && object && packaged(object) && runtime.gdk_packages().references() == 2 &&
              !independent.gdk_packages().is_packaged() && independent.gdk_packages().references() == 1,
          "the exact package interface owns a validated loose-process identity independently per runtime");
    std::array<char, 64> buffer;
    buffer.fill(static_cast<char>(0xcc));
    const auto saved_error = errno;
    errno = EDOM;
    crt_error() = 61;
    set_last_error(91);
    const auto copied = identifier(object, buffer.size() - 2, buffer.data() + 1);
    check(copied == 0 && std::strcmp(buffer.data() + 1, "Ambrosia.Example") == 0 &&
              static_cast<unsigned char>(buffer.front()) == 0xcc &&
              static_cast<unsigned char>(buffer[18]) == 0xcc && errno == EDOM && crt_error() == 61 &&
              get_last_error() == 91,
          "the package identifier comes from actual original metadata, includes its NUL and preserves "
          "adjacent storage and native/CRT/Windows errors");
    buffer.fill(static_cast<char>(0xcc));
    const auto small = identifier(object, 16, buffer.data());
    const auto missing_output = identifier(object, buffer.size(), nullptr);
    const auto no_package = independent.gdk_packages().identifier(buffer.size(), buffer.data());
    check(small == static_cast<std::int32_t>(0x8007007au) && missing_output == pointer_error &&
              no_package == no_current_package && static_cast<unsigned char>(buffer[0]) == 0xcc,
          "short/missing buffers and an unconfigured process fail without publishing a fabricated identity");
    void *unknown = nullptr;
    const auto unknown_query = qi(object, &unknown_interface_id, &unknown);
    const auto unknown_release = release(unknown);
    const Guid absent{};
    unknown = reinterpret_cast<void *>(1);
    const auto absent_query = query(&package_class_id, &absent, &unknown);
    check(unknown_query == 0 && unknown_release == 2 && absent_query == no_interface && !unknown,
          "package IUnknown counting and unknown-version refusal share the actual owned object");
    bool immutable = false;
    try {
        runtime.gdk_packages().bind(runtime.files());
    } catch (const std::runtime_error &) {
        immutable = true;
    }
    check(immutable && packaged(object),
          "a process package identity cannot be silently rebound after publication");
    const auto mismatch_root = root / "mismatch";
    std::filesystem::create_directories(mismatch_root);
    write(mismatch_root / "MicrosoftGame.Config", descriptor("Game", "CN=Example", "1.2.3.4"));
    write(mismatch_root / "AppxManifest.xml", descriptor("Package", "CN=Other", "1.2.3.4"));
    ReadonlyFiles mismatch_files;
    mismatch_files.bind(mismatch_root, image);
    GdkPackages mismatch;
    bool rejected = false;
    try {
        mismatch.bind(mismatch_files);
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    bool recovered = false;
    if (rejected && !mismatch.is_packaged()) {
        write(mismatch_root / "AppxManifest.xml", descriptor("Package", "CN=Example", "1.2.3.4"));
        mismatch.bind(mismatch_files);
        recovered = mismatch.is_packaged();
    }
    check(rejected && recovered, "conflicting publishers cannot publish partial package state and a "
                                 "corrected descriptor can bind afterward");
    const auto version_root = root / "versions";
    std::filesystem::create_directories(version_root);
    write(version_root / "MicrosoftGame.Config", descriptor("Game", "CN=Example", "1.2.3.4"));
    write(version_root / "AppxManifest.xml", descriptor("Package", "CN=Example", "1.2.3.5"));
    ReadonlyFiles version_files;
    version_files.bind(version_root, image);
    GdkPackages versions;
    bool bad_version = false;
    try {
        versions.bind(version_files);
    } catch (const std::runtime_error &) {
        bad_version = true;
    }
    check(bad_version && !versions.is_packaged(),
          "matching names cannot conceal conflicting package versions");
    const auto empty_root = root / "empty";
    std::filesystem::create_directories(empty_root);
    ReadonlyFiles empty_files;
    empty_files.bind(empty_root, image);
    GdkPackages empty;
    empty.bind(empty_files);
    check(!empty.is_packaged(),
          "absence of descriptors represents an unpackaged process without guessed defaults");
    const auto released = release(object);
    check(released == 1 && runtime.gdk_packages().references() == 1,
          "the package runtime pin retains immutable metadata after the last guest interface release");
    errno = saved_error;
}
