#if defined(__linux__)
std::string inspection_page_permissions(const std::byte *address) {
    std::ifstream maps("/proc/self/maps");
    std::string line;
    const auto pointer = reinterpret_cast<std::uintptr_t>(address);
    while (std::getline(maps, line)) {
        std::istringstream fields(line);
        std::uintptr_t begin = 0, end = 0;
        char separator = 0;
        std::string permissions;
        fields >> std::hex >> begin >> separator >> end >> permissions;
        if (fields && separator == '-' && pointer >= begin && pointer < end) {
            return permissions;
        }
    }
    return {};
}

bool inspection_page_is_read_only(const std::byte *address) {
    return inspection_page_permissions(address).starts_with("r--");
}
#endif

Bytes inspection_fixture() {
    auto bytes = fixture();
    put<std::uint32_t>(bytes, 0x98 + 128, 0x21f0);
    put<std::uint32_t>(bytes, 0x98 + 132, 16);
    put<std::uint32_t>(bytes, 0x98 + 136, 0x21a0);
    put<std::uint32_t>(bytes, 0x98 + 140, 12);
    put<std::uint32_t>(bytes, 0x5a0, 0x1000);
    put<std::uint32_t>(bytes, 0x5a4, 0x1040);
    put<std::uint32_t>(bytes, 0x5a8, 0x21b0);
    put<std::uint32_t>(bytes, 0x5b0, 1); // Version-one, zero-code unwind header.
    put<std::uint32_t>(bytes, 0x98 + 160, 0x21c0);
    put<std::uint32_t>(bytes, 0x98 + 164, 28);
    put<std::uint32_t>(bytes, 0x98 + 192, 0x21e0);
    put<std::uint32_t>(bytes, 0x98 + 196, 16);
    put<std::uint32_t>(bytes, 0x5e0, 16);
    return bytes;
}

void inspection_checks() {
    using ambrosia::pe::Image;
    using ambrosia::pe::ImageLimits;
    using ambrosia::pe::LoadMode;
    using ambrosia::pe::ModuleGraph;
    using ambrosia::pe::ModuleLimits;
    ImageLimits limits;
    limits.mode = LoadMode::inspect;
    const auto bytes = inspection_fixture();
    check(rejected(bytes), "executable loading still rejects unsupported exception metadata");
    auto inspected = Image::load(bytes, {}, 0, limits);
    check(inspected.imports().size() == 1 && inspected.imports()[0].name == "sum6",
          "inspection retains unresolved imports without fake bindings");
    std::uint64_t address = 1;
    std::memcpy(&address, inspected.base() + 0x2080, sizeof(address));
    check(address == 0, "unresolved inspected IAT cell remains null");
    check(runtime_rejected([&] { (void)inspected.start(); }), "inspection-only image rejects initialization");
    check(runtime_rejected([&] { (void)inspected.run_bootstrap(); }),
          "inspection-only image rejects bootstrap execution");
    auto moved = std::move(inspected);
    check(runtime_rejected([&] { (void)moved.start(); }), "inspection restriction survives image move");
    auto resolved = Image::load(fixture(), resolve, 0, limits);
    check(runtime_rejected([&] { (void)resolved.run_bootstrap(); }),
          "resolved imports cannot make an inspected image executable");
#if defined(__linux__)
    check(inspection_page_is_read_only(resolved.base() + 0x1000) &&
              inspection_page_is_read_only(resolved.base() + 0x2000),
          "inspection code and data pages are read-only and non-executable");
#endif
    auto certified = fixture();
    certified.resize(0x850);
    put<std::uint32_t>(certified, 0x98 + 144, 0x800);
    put<std::uint32_t>(certified, 0x98 + 148, 0x50);
    auto certificate = Image::load(certified, {}, 0, limits);
    check(runtime_rejected([&] { (void)certificate.start(); }),
          "opaque certificate inspection does not authenticate or enable execution");
    put<std::uint32_t>(certified, 0x98 + 148, 0x51);
    check(runtime_rejected([&] { (void)Image::load(certified, {}, 0, limits); }),
          "certificate extent is checked against file size rather than image size");

    ModuleLimits graph_limits;
    graph_limits.image = limits;
    ModuleGraph graph({}, {}, graph_limits);
    graph.add("app.exe", bytes);
    graph.load("app.exe");
    const auto statistics = graph.statistics();
    check(statistics.inspection_only && statistics.unresolved_imports == 1 &&
              statistics.unwind_records == 1 &&
              statistics.unsupported_directories == ((1u << 2) | (1u << 3) | (1u << 6) | (1u << 10)),
          "inspection graph reports aggregate service and directory blockers");
    check(runtime_rejected([&] { (void)graph.start(); }),
          "inspection graph refuses execution before any module starts");
    ModuleGraph executable;
    executable.add("app.exe", fixture());
    check(runtime_rejected([&] { executable.load("app.exe"); }),
          "executable graph still rejects unresolved imports");
    auto bad_directory = [&](std::size_t offset, std::uint32_t value) {
        auto malformed = bytes;
        put<std::uint32_t>(malformed, offset, value);
        return runtime_rejected([&] { (void)Image::load(malformed, {}, 0, limits); });
    };
    check(bad_directory(0x98 + 164, 27), "inspection rejects a partial debug directory");
    check(bad_directory(0x5e0, 17), "inspection rejects oversized load configuration");
    check(bad_directory(0x5e0, 0), "inspection rejects missing load-configuration size");
    check(bad_directory(0x5a8, 0x4000), "inspection validates owned unwind references");
    check(bad_directory(0x98 + 136, 0x4000), "inspection retains directory extent checks");

    lifecycle_events.clear();
    auto tls = Image::load(tls_fixture(true), resolve, 0, limits);
    check(runtime_rejected([&] { (void)tls.start(); }) && lifecycle_events.empty(),
          "inspection rejects a TLS DLL before attach callbacks");
    const auto root = std::filesystem::temp_directory_path() / "ambrosia-inspection-tests";
    std::filesystem::create_directories(root);
    const auto path = root / "inspect.exe";
    write_fixture(path, bytes);
    std::ostringstream output, errors;
    const std::vector<std::filesystem::path> arguments{"--inspect", path};
    check(ambrosia::run_native_command(arguments, output, errors) == 0 && errors.str().empty() &&
              output.str().find("\"inspection_only\":true") != std::string::npos &&
              output.str().find("\"unresolved_imports\":1") != std::string::npos &&
              output.str().find("\"unwind_records\":1") != std::string::npos &&
              output.str().find("bootstrap_result") == std::string::npos,
          "CLI separates successful inspection from execution readiness");
    std::filesystem::remove_all(root);
}
