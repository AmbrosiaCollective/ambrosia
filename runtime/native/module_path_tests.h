void module_path_checks() {
    using namespace ambrosia;
    using namespace services;
    const std::filesystem::path origin(u8"/work/tmp/fixture dir/App-λ-👋.exe");
    const std::filesystem::path provider_path("/work/tmp/package/PrOvIdEr.DLL");
    pe::ModuleGraph graph(resolve);
    check(runtime_rejected([&] { graph.add("bad.exe", fixture(), "relative.exe"); }),
          "source-origin metadata rejects relative paths before registering an image");
    graph.add("app.exe", fixture(), origin);
    graph.add("provider.dll", provider_fixture(), provider_path);
    graph.add("unmapped.dll", provider_fixture(), "/work/tmp/package/unmapped.dll");
    check(!graph.loaded_module_path(), "source-origin lookup does not load an image or open its file");
    graph.load("app.exe");
    (void)graph.export_address("provider.dll", named_symbol("named_value"));
    const auto root = graph.loaded_module();
    const auto provider = graph.loaded_module("provider.dll");
    check(graph.loaded_module_path() == origin && graph.loaded_module_path(root) == origin &&
              graph.loaded_module_path(provider) == provider_path &&
              !graph.loaded_module_path(provider + 1) && graph.statistics().modules == 2,
          "module origins retain exact source case and distinguish root/base handles without mapping "
          "source-only images");
    pe::ModuleGraph memory_only(resolve);
    memory_only.add("memory.exe", fixture());
    memory_only.load("memory.exe");
    check(runtime_rejected([&] { (void)memory_only.loaded_module_path(); }),
          "an in-memory image without a source origin cannot fabricate a file path");
    Runtime runtime;
    Scope scope(runtime, &graph);
    using Wide = std::uint32_t(AMBROSIA_GUEST_ABI *)(void *, char16_t *, std::uint32_t);
    using Narrow = std::uint32_t(AMBROSIA_GUEST_ABI *)(void *, char *, std::uint32_t);
    const auto address =
        runtime.resolve(service_symbol("GetModuleFileNameW", "api-ms-win-core-libraryloader-l1-2-0.dll"));
    const auto wide = reinterpret_cast<Wide>(address);
    const auto narrow =
        reinterpret_cast<Narrow>(runtime.resolve(service_symbol("GetModuleFileNameA", "kernel32.dll")));
    check(address && address == runtime.resolve(service_symbol("GetModuleFileNameW", "kernel32.dll")) &&
              address == runtime.resolve(service_symbol("GetModuleFileNameW", "kernelbase.dll")),
          "module filename aliases share the typed Windows capacity/count ABI");
    const auto saved_error = errno;
    errno = E2BIG;
    crt_error() = 19;
    set_last_error(87);
    std::array<char16_t, 260> output;
    output.fill(u'!');
    const auto expected = origin.u16string();
    const auto count = wide(nullptr, output.data() + 1, 258);
    check(count == expected.size() && std::u16string(output.data() + 1) == expected &&
              output.front() == u'!' && output[count + 2] == u'!' && output.back() == u'!',
          "wide module filename returns exact UTF-16 units including non-BMP pairs and touches only its "
          "terminated result");
    check(wide(reinterpret_cast<void *>(root), output.data(), output.size()) == expected.size() &&
              std::u16string(output.data()) == expected,
          "null and explicit root-module handles return the same registered origin");
    std::array<char, 260> bytes;
    bytes.fill('!');
    const auto utf8 = origin.u8string();
    const std::string expected_bytes(reinterpret_cast<const char *>(utf8.data()), utf8.size());
    check(narrow(nullptr, bytes.data(), bytes.size()) == expected_bytes.size() &&
              std::string(bytes.data()) == expected_bytes,
          "narrow module filenames follow the existing explicit UTF-8 bootstrap path policy");
    check(wide(reinterpret_cast<void *>(provider), output.data(), output.size()) ==
                  provider_path.u16string().size() &&
              std::u16string(output.data()) == provider_path.u16string(),
          "loaded DLL queries preserve their actual source path rather than lowercasing the host filename");
    check(errno == E2BIG && crt_error() == 19 && get_last_error() == 87,
          "successful module-path queries preserve native/guest CRT errno and existing Win32 LastError");
    std::array<char16_t, 6> short_output{u'L', u'x', u'x', u'x', u'x', u'R'};
    check(wide(nullptr, short_output.data() + 1, 4) == 4 && get_last_error() == 122 &&
              short_output[0] == u'L' && short_output[1] == expected[0] && short_output[3] == expected[2] &&
              short_output[4] == 0 && short_output[5] == u'R',
          "short module-path buffers return capacity, terminate and report insufficient buffer without "
          "crossing guards");
    bytes.fill('!');
    check(narrow(nullptr, bytes.data(), 1) == 1 && bytes[0] == 0 && bytes[1] == '!' &&
              wide(nullptr, nullptr, 0) == 0 && get_last_error() == 122,
          "one-byte and zero-capacity path queries honor destination and error bounds");
    std::vector<char16_t> exact(expected.size() + 1);
    check(wide(nullptr, exact.data(), exact.size()) == expected.size() && exact.back() == 0,
          "an exact filename-plus-terminator buffer returns the full count");
    output.fill(u'!');
    check(!wide(reinterpret_cast<void *>(provider + 1), output.data(), output.size()) &&
              get_last_error() == 126 && output.front() == u'!',
          "interior or unknown handles do not impersonate module bases or write an invented filename");

    auto caller = fixture();
    text(caller, 0x440, "kernel32.dll");
    text(caller, 0x4a2, "GetModuleFileNameW");
    // Null module; output=RVA2110; capacity=64 UTF-16 units; return DWORD length.
    constexpr unsigned char code[] = {0x48, 0x83, 0xec, 0x28, 0x31, 0xc9, 0x48, 0x8d, 0x15, 0,
                                      0,    0,    0,    0x41, 0xb8, 64,   0,    0,    0,    0xff,
                                      0x15, 0,    0,    0,    0,    0x48, 0x83, 0xc4, 0x28, 0xc3};
    std::memcpy(caller.data() + 0x200, code, sizeof(code));
    put<std::uint32_t>(caller, 0x209, 0x2110 - (0x1000 + 13));
    put<std::uint32_t>(caller, 0x215, 0x2080 - (0x1000 + 25));
    auto image = pe::Image::load(caller, [&](const auto &symbol) { return runtime.resolve(symbol); });
    check(image.run_bootstrap() == expected.size() &&
              std::u16string(reinterpret_cast<const char16_t *>(image.base() + 0x2110)) == expected,
          "an original PE receives its scoped module origin through register arguments and a UTF-16 guest "
          "destination");
    errno = saved_error;
}
