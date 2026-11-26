void package_installation_checks() {
    using namespace ambrosia;
    using namespace services;
    const std::string identity =
        "<Identity Name='Ambrosia.Inventory' Publisher='CN=Example' Version='1.2.3.4'/>";
    const auto document = [&](std::string_view entries) {
        return "<Package>" + identity + "<Inventory>" + std::string(entries) + "</Inventory></Package>";
    };
    const auto parse = [](std::string_view text) {
        return parse_package_inventory(std::as_bytes(std::span(text.data(), text.size())));
    };
    const auto rejected = [&](std::string_view text) {
        try {
            (void)parse(text);
            return false;
        } catch (const std::runtime_error &) {
            return true;
        }
    };
    const auto expected = document("<File Path='a.bin' Size='3'/><File Path='nested/b.bin' Size='4'/>"
                                   "<File Path='empty.bin' Size='0'/>");
    const auto inventory = parse(expected);
    check(inventory.identity.name == "Ambrosia.Inventory" && inventory.files.size() == 3 &&
              inventory.files[1].path == "nested/b.bin" && inventory.files[2].bytes == 0,
          "the original expected inventory preserves exact identity, relative UTF8 paths and zero sizes");
    const auto unicode = parse(document("<File Path='caf\xc3\xa9.bin' Size='0'/>"));
    check(unicode.files[0].path == "caf\xc3\xa9.bin",
          "inventory paths preserve valid Unicode rather than developer platform encodings");
    check(rejected(document("")) && rejected("<Package>" + identity + "</Package>") &&
              rejected("<Package>" + identity + "<Inventory/><Inventory/></Package>") &&
              rejected(document("<File Path='a' Size='1'><File Path='b' Size='2'/></File>")) &&
              rejected(document("unexpected<File Path='a' Size='1'/>")),
          "empty, missing, duplicate, nested and non-metadata inventories fail without partial state");
    bool unsafe_paths_rejected = true;
    for (const auto *path :
         {"/root", "../a", "a/../b", "a//b", "a/./b", "a/", "C:/a", "a\\b", "a*", "a.", "a ", "a&#9;b"}) {
        unsafe_paths_rejected =
            unsafe_paths_rejected && rejected(document(std::string("<File Path='") + path + "' Size='1'/>"));
    }
    check(unsafe_paths_rejected && rejected(document("<File Path='a' Size='1'/><File Path='A' Size='1'/>")),
          "absolute/traversing/aliased/control/wildcard paths and ASCII case duplicates are rejected");
    check(rejected(document("<File Path='a' Size='-1'/>")) &&
              rejected(document("<File Path='a' Size='+1'/>")) &&
              rejected(document("<File Path='a' Size='1x'/>")) &&
              rejected(document("<File Path='a' Size=''/>")) &&
              rejected(document("<File Path='a' Size='9223372036854775808'/>")) &&
              rejected(document("<File Path='a' Size='1' Extra='yes'/>")),
          "size syntax, signed-offset overflow and undeclared attributes fail instead of truncating");
    check(rejected("<!DOCTYPE Package [<!ENTITY name 'a'>]>" + expected) &&
              rejected(document("<File Path='" + std::string(1025, 'x') + "' Size='0'/>")),
          "DTD input and overlong paths remain outside the bounded metadata contract");
    std::string many;
    for (unsigned index = 0; index < 1025; ++index) {
        many += "<File Path='f" + std::to_string(index) + "' Size='0'/>";
    }
    check(rejected(document(many)), "inventory file count is bounded before host inspection");

    const auto root = std::filesystem::path("/work/package-installation");
    const auto package = root / "assets";
    const auto image = root / "bin" / "fixture.exe";
    std::filesystem::create_directories(package / "nested");
    std::filesystem::create_directories(image.parent_path());
    const auto write = [](const auto &path, std::string_view text) {
        std::ofstream stream(path, std::ios::binary);
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!stream) {
            throw std::runtime_error("cannot write original inventory fixture");
        }
    };
    write(image, "original image fixture");
    write(package / "a.bin", "abc");
    write(package / "nested/b.bin", "defg");
    write(package / "empty.bin", "");
    ReadonlyFiles files;
    files.bind(package, image);
    PackageInstallation installation(inventory, files);
    auto observed = installation.inspect();
    check(observed.completed() && observed.total_bytes == 7 && observed.installed_bytes == 7 &&
              observed.expected_files == 3 && observed.ready_files == 3,
          "complete original files produce actual logical-size readiness including a required empty file");
    write(package / "a.bin", "ab");
    observed = installation.inspect();
    check(!observed.completed() && observed.total_bytes == 7 && observed.installed_bytes == 4 &&
              observed.ready_files == 2,
          "a short file contributes no completed-file bytes and cannot make the package ready");
    write(package / "a.bin", "abcd");
    check(!installation.inspect().completed(), "an oversized file is also a mismatch, not installed data");
    write(package / "a.bin", "abc");
    std::filesystem::remove(package / "empty.bin");
    observed = installation.inspect();
    check(!observed.completed() && observed.installed_bytes == observed.total_bytes &&
              observed.ready_files == 2,
          "equal byte totals cannot hide an absent required zero-length file");
    std::filesystem::create_directory(package / "empty.bin");
    check(!installation.inspect().completed(), "a directory cannot substitute for a required regular file");
    std::filesystem::remove(package / "empty.bin");
    write(package / "empty.bin", "");
    std::filesystem::rename(package / "nested", package / "moved");
    check(!installation.inspect().completed(), "a missing parent directory reports incomplete installation");
    std::filesystem::rename(package / "moved", package / "nested");
    check(installation.inspect().completed(), "a fresh observation sees restored files without rebinding");

    bool overflow_rejected = false;
    try {
        auto overflow = inventory;
        overflow.files[0].bytes = std::numeric_limits<std::int64_t>::max();
        (void)PackageInstallation(overflow, files);
    } catch (const std::runtime_error &) {
        overflow_rejected = true;
    }
    check(overflow_rejected, "aggregate expected bytes cannot overflow the supported signed-offset range");
    write(package / "MicrosoftGame.Config", "<Game>" + identity + "</Game>");
    GdkPackages packages;
    packages.bind(files);
    check(!packages.installation(), "process identity alone never asserts package readiness");
    const auto wrong = document("<File Path='a.bin' Size='3'/>");
    auto wrong_identity = wrong;
    wrong_identity.replace(wrong_identity.find("1.2.3.4"), 7, "1.2.3.5");
    bool mismatch = false;
    try {
        packages.bind_installation(std::as_bytes(std::span(wrong_identity.data(), wrong_identity.size())),
                                   files);
    } catch (const std::runtime_error &) {
        mismatch = true;
    }
    check(mismatch && !packages.installation(),
          "inventory version conflicts cannot publish a partially configured readiness source");
    packages.bind_installation(std::as_bytes(std::span(expected.data(), expected.size())), files);
    bool rebound = false;
    try {
        packages.bind_installation(std::as_bytes(std::span(expected.data(), expected.size())), files);
    } catch (const std::runtime_error &) {
        rebound = true;
    }
    check(rebound && packages.installation()->inspect().completed(),
          "a corrected inventory binds once and cannot silently replace published expectations");
    std::vector<std::shared_ptr<host::ReadonlyFile>> leases;
    for (unsigned index = 0; index < 32; ++index) {
        const auto opened = files.open("a.bin");
        if (opened.error || !opened.file) {
            throw std::runtime_error("cannot fill original file lease fixture");
        }
        leases.push_back(opened.file);
    }
    check(packages.installation()->inspect().completed(),
          "metadata observations do not consume the guest's 32 shared asset-file leases");
    leases.clear();
    std::filesystem::remove(package / "a.bin");
    std::filesystem::create_symlink(image, package / "a.bin");
    bool symlink_rejected = false;
    try {
        (void)installation.inspect();
    } catch (const std::runtime_error &) {
        symlink_rejected = true;
    }
    check(symlink_rejected, "a substituted symlink fails confinement instead of supplying readiness");
    std::filesystem::remove(package / "a.bin");
    write(package / "a.bin", "abc");
    std::shared_ptr<PackageInstallation> retained;
    {
        ReadonlyFiles short_lived;
        short_lived.bind(package, image);
        retained = std::make_shared<PackageInstallation>(inventory, short_lived);
    }
    check(retained->inspect().completed(),
          "installation sources retain confined directory references without borrowing frontend lifetime");

    std::ostringstream output;
    std::ostringstream errors;
    const auto invalid_mode = run_native_command(
        std::vector<std::filesystem::path>{"--inspect", image, "--package-inventory", root / "inventory"},
        output, errors);
    const auto duplicate = run_native_command(
        std::vector<std::filesystem::path>{"--run-bootstrap", image, "--package-inventory", root / "one",
                                           "--package-inventory", root / "two"},
        output, errors);
    check(invalid_mode == 2 && duplicate == 2,
          "inventory input is explicit, unique and refused in inspection-only mode before any entry");
}
