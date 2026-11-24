void write_fixture(const std::filesystem::path &path, const Bytes &bytes) {
    std::ofstream stream(path, std::ios::binary);
    if (!stream.write(reinterpret_cast<const char *>(bytes.data()), bytes.size())) {
        throw std::runtime_error("cannot write synthetic package fixture");
    }
}

void package_checks() {
    using ambrosia::host::PackageLimits;
    using ambrosia::host::PackageModules;
    const auto root = std::filesystem::temp_directory_path() / "ambrosia-package-tests";
    const auto package = root / "package with spaces";
    std::filesystem::create_directories(package);

    struct Cleanup {
        std::filesystem::path root;

        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }
    } cleanup{root};

    const auto provider_bytes = provider_fixture();
    write_fixture(package / "PrOvIdEr.DLL", provider_bytes);
    write_fixture(package / "unused.dll", Bytes{std::byte{0xff}});
    write_fixture(package / "asset.rpf", Bytes{std::byte{0}});
    PackageModules files(package);
    check(files.files_read() == 0 && files.bytes_read() == 0, "package enumeration reads no file content");
    check(!files.read("missing.dll"), "missing package module is explicit");
    auto loaded = files.read("PROVIDER.dll");
    check(loaded && *loaded == provider_bytes, "package lookup uses actual path with case-folded basename");
    check(files.files_read() == 1 && files.bytes_read() == provider_bytes.size(),
          "package reports only requested content reads");
    check(runtime_rejected([&] { (void)files.read("../provider.dll"); }), "package read rejects traversal");
    check(runtime_rejected([&] { (void)files.read(std::string("provider.dll\0suffix", 19)); }),
          "package read rejects embedded NUL");
    PackageLimits short_file;
    short_file.file_bytes = provider_bytes.size() - 1;
    PackageModules bounded_file(package, short_file);
    check(runtime_rejected([&] { (void)bounded_file.read("provider.dll"); }) &&
              bounded_file.files_read() == 0,
          "per-file budget fails without recording a successful read");
    PackageLimits total;
    total.total_bytes = provider_bytes.size();
    PackageModules bounded_total(package, total);
    (void)bounded_total.read("provider.dll");
    check(runtime_rejected([&] { (void)bounded_total.read("provider.dll"); }),
          "aggregate package byte budget enforced");
    PackageLimits entries;
    entries.directory_entries = 1;
    check(runtime_rejected([&] { PackageModules too_many(package, entries); }),
          "package directory entry budget enforced");
    write_fixture(package / "provider.dll", provider_bytes);
    if (!std::filesystem::equivalent(package / "PrOvIdEr.DLL", package / "provider.dll")) {
        check(runtime_rejected([&] { PackageModules ambiguous(package); }),
              "ambiguous DLL case variants rejected");
        std::filesystem::remove(package / "provider.dll");
    } else {
        PackageModules same_file(package);
        check(bool(same_file.read("provider.dll")), "case aliases resolve one physical package file");
    }
    write_fixture(root / "outside.dll", provider_bytes);
    std::filesystem::create_symlink(root / "outside.dll", package / "escape.dll");
    check(runtime_rejected([&] { PackageModules escaped(package); }),
          "package DLL symlink rejected before reading target");
    std::filesystem::remove(package / "escape.dll");

    auto caller = fixture();
    text(caller, 0x440, "provider.dll");
    text(caller, 0x4a2, "named_value");
    PackageModules lazy(package);
    ambrosia::pe::ModuleGraph graph({}, [&](const auto &name) { return lazy.read(name); });
    graph.add("app.exe", caller);
    graph.load("app.exe");
    check(graph.initialization_order() == std::vector<std::string>{"provider.dll", "app.exe"},
          "lazy provider dependency enters initialization order");
    const auto statistics = graph.statistics();
    check(statistics.modules == 2 && statistics.imports == 1 && lazy.files_read() == 1,
          "lazy graph reads one dependency and ignores invalid unused DLL");
    {
        auto execution = graph.start();
        check(execution.run_bootstrap() == 42,
              "package-backed native EXE-to-DLL call returns expected value");
    }
    std::size_t provider_calls = 0;
    ambrosia::pe::ModuleGraph shim_first(resolve, [&](const auto &) -> std::optional<Bytes> {
        ++provider_calls;
        throw std::runtime_error("explicit shim should resolve first");
    });
    shim_first.add("app.exe", fixture());
    shim_first.load("app.exe");
    check(provider_calls == 0, "explicit shim resolver takes precedence over package discovery");

    const auto image_path = root / std::filesystem::path(u8"entry café.exe");
    const auto work = root / "work cache";
    write_fixture(image_path, caller);
    auto invoke = [&](const std::vector<std::filesystem::path> &arguments,
                      std::string *text_output = nullptr) {
        std::ostringstream output, errors;
        const auto code = ambrosia::run_native_command(arguments, output, errors);
        if (text_output) {
            *text_output = output.str();
        }
        check(output.str().find(root.string()) == std::string::npos &&
                  errors.str().find(root.string()) == std::string::npos,
              "CLI output omits private input paths");
        return code;
    };
    std::string output;
    check(invoke({"--inspect", image_path, "--package", package, "--work", work}, &output) == 0 &&
              output.find("\"modules\":2") != std::string::npos &&
              output.find("\"files_read\":2") != std::string::npos &&
              output.find("bootstrap_result") == std::string::npos,
          "CLI inspects package-backed modules without executing entry");
    check(invoke({"--run-bootstrap", image_path, "--package", package}, &output) == 0 &&
              output.find("\"bootstrap_result\":42") != std::string::npos,
          "CLI bootstrap executes through package-backed module graph");
    auto tls_entry = tls_fixture();
    put<std::uint32_t>(tls_entry, 0x98 + 120, 0);
    put<std::uint32_t>(tls_entry, 0x98 + 124, 0);
    jump_to_host(tls_entry, 0x300, reinterpret_cast<std::uintptr_t>(&graph_callback));
    const auto tls_path = root / "tls-entry.exe";
    write_fixture(tls_path, tls_entry);
    graph_events.clear();
    check(invoke({"--inspect", tls_path, "--package", package}) == 0 && graph_events.empty(),
          "CLI inspect does not run TLS initialization or entry code");
    graph_events.clear();
    check(invoke({"--run-bootstrap", tls_path, "--package", package}, &output) == 0 &&
              output.find("\"bootstrap_result\":42") != std::string::npos &&
              graph_events == std::vector<std::pair<std::uint32_t, std::uint32_t>>{{41, 1}, {42, 0}},
          "CLI run delivers TLS attach and detach around entry");
    put<std::uint16_t>(tls_entry, 0x96, 0x2022);
    const auto dll_path = root / "tls-entry.dll";
    write_fixture(dll_path, tls_entry);
    graph_events.clear();
    check(invoke({"--run-bootstrap", dll_path, "--package", package}) == 1 && graph_events.empty(),
          "CLI rejects DLL bootstrap before initialization executes");
    check(!std::filesystem::exists(work), "CLI creates no work directory or package output");
    check(invoke({"--help"}) == 0, "CLI help does not require image files");
    check(invoke({}) == 2, "CLI rejects absent mode and image");
    check(invoke({"--inspect", "--package", package}) == 2, "CLI rejects another flag as a path value");
    check(invoke({"--inspect", image_path, "--run-bootstrap", image_path}) == 2,
          "CLI rejects conflicting modes");
    check(invoke({"--inspect", image_path, "--package", package, "--package", package}) == 2,
          "CLI rejects duplicate package option");
    check(invoke({"--unknown", image_path}) == 2, "CLI rejects unknown option");
    check(invoke({"--inspect", root / "missing.exe"}) == 1, "CLI reports unavailable input without paths");
    check(invoke({"--inspect", image_path, "--package", package, "--work", package / "cache"}) == 1,
          "CLI refuses work output inside read-only package");
}
