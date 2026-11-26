void ordinal_service_checks() {
    using namespace ambrosia;
    using namespace services;
    const auto target = reinterpret_cast<std::uintptr_t>(&sum6);
    ImportRegistry registry;
    registry.add("ordinal.dll", "sum6", target);
    auto symbol = service_symbol("ignored", "ordinal.dll");
    symbol.by_ordinal = true;
    symbol.ordinal = 115;
    check(!registry.resolve(symbol), "unregistered service ordinals remain unresolved");
    registry.add_ordinal("ORDINAL.dll", 115, "sum6");
    check(registry.resolve(symbol) == target,
          "an explicit ordinal alias resolves its registered named target");
    registry.add("ordinal.dll", "other", target);
    check(runtime_rejected([&] { registry.add_ordinal("ordinal.dll", 115, "other"); }) &&
              runtime_rejected([&] { registry.add_ordinal("ordinal.dll", 116, "sum6"); }) &&
              runtime_rejected([&] { registry.add_ordinal("ordinal.dll", 0, "other"); }) &&
              runtime_rejected([&] { registry.add_ordinal("ordinal.dll", 117, "missing"); }),
          "ordinal registration rejects duplicate IDs/names, zero and missing named targets");
    ImportRegistry bounded;
    for (unsigned index = 0; index < 4095; ++index) {
        bounded.add("bounded.dll", "entry" + std::to_string(index), target);
    }
    bounded.add_ordinal("bounded.dll", 115, "entry0");
    check(runtime_rejected([&] { bounded.add("bounded.dll", "overflow", target); }) &&
              runtime_rejected([&] { bounded.add_ordinal("bounded.dll", 116, "entry1"); }),
          "named registrations and public ordinal aliases share the global service-entry budget");
    ServiceModuleImage sparse(
        "sparse.dll", {{"sum6", target, {}, 115}, {"automatic", target, {}, 1}, {"another", target, {}}});
    const auto &exports = sparse.exports();
    check(exports.ordinals.size() == 3 && !exports.ordinals.contains(114) &&
              exports.ordinals.at(115).rva == exports.names.at("sum6").rva &&
              exports.ordinals.at(2).rva == exports.names.at("another").rva,
          "real sparse PE exports reserve explicit ordinals before assigning private image slots");
    check(runtime_rejected([&] {
              ServiceModuleImage duplicate("bad.dll", {{"first", target, {}, 9}, {"second", target, {}, 9}});
          }),
          "PE service metadata rejects duplicate explicit export ordinals");
    ServiceModuleImage highest("highest.dll", {{"sum6", target, {}, 65535}});
    check(highest.exports().ordinals.size() == 1 && highest.exports().ordinals.contains(65535) &&
              highest.bytes().size() <= 512 * 1024,
          "the highest supported ordinal keeps sparse holes empty within the image byte budget");
    NativeModules modules(registry);
    const auto handle = modules.handle("ordinal.dll");
    const auto callable = modules.entrypoint(target);
    check(
        modules.export_address(handle, std::uint16_t{115}) == callable &&
            modules.export_address(handle, "sum6") == callable &&
            modules.export_address(handle, std::uint16_t{114}) == 0 &&
            !modules.export_address(1, std::uint16_t{115}).has_value(),
        "native module ordinal lookup preserves canonical identity and distinguishes holes/invalid handles");
    const auto *base = reinterpret_cast<const std::byte *>(handle);
    const auto bytes = std::span(base, std::size_t{4096});
    const auto nt = pe::detail::read<std::uint32_t>(bytes, 0x3c);
    const auto size = pe::detail::read<std::uint32_t>(bytes, nt + 24 + 56);
    const auto export_rva = pe::detail::read<std::uint32_t>(bytes, nt + 24 + 112);
    const auto export_size = pe::detail::read<std::uint32_t>(bytes, nt + 24 + 116);
    const auto metadata =
        pe::parse_exports(std::span(base, static_cast<std::size_t>(size)), export_rva, export_size);
    check(metadata.ordinals.at(115).forwarder == metadata.names.at("sum6").forwarder,
          "the facade's actual PE ordinal table forwards to the same native entry as its named export");
    auto caller = fixture();
    text(caller, 0x440, "ordinal.dll");
    put<std::uint64_t>(caller, 0x460, 0x8000000000000073ull);
    auto image = pe::Image::load(caller, [&](const auto &import) {
        const auto address = registry.resolve(import);
        return address ? modules.entrypoint(address) : 0;
    });
    saw_arguments = false;
    const auto returned = image.run_bootstrap();
    check(returned == 21 && saw_arguments, "an original PE ordinal import calls the canonical native "
                                           "trampoline with six Microsoft ABI arguments");
}

void winsock_lifecycle_checks() {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    Runtime other;
    Scope scope(runtime);
    using Startup = std::int32_t(AMBROSIA_GUEST_ABI *)(std::uint16_t, void *);
    using Cleanup = std::int32_t(AMBROSIA_GUEST_ABI *)();
    using GetError = std::int32_t(AMBROSIA_GUEST_ABI *)();
    using SetError = void(AMBROSIA_GUEST_ABI *)(std::int32_t);
    auto startup_symbol = service_symbol("WSAStartup", "ws2_32.dll");
    const auto named = runtime.resolve(startup_symbol);
    startup_symbol.by_ordinal = true;
    startup_symbol.ordinal = 115;
    const auto startup = reinterpret_cast<Startup>(runtime.resolve(startup_symbol));
    const auto cleanup =
        reinterpret_cast<Cleanup>(runtime.resolve(service_symbol("WSACleanup", "ws2_32.dll")));
    const auto get =
        reinterpret_cast<GetError>(runtime.resolve(service_symbol("WSAGetLastError", "ws2_32.dll")));
    const auto set =
        reinterpret_cast<SetError>(runtime.resolve(service_symbol("WSASetLastError", "ws2_32.dll")));
    check(reinterpret_cast<std::uintptr_t>(startup) == named,
          "WSAStartup name and ordinal 115 bind to one canonical callable address");
    const auto saved_error = errno;
    errno = EDOM;
    crt_error() = 61;
    set(71);
    std::array<std::byte, 414> guarded;
    guarded.fill(std::byte{0xcc});
    const auto null_output = startup(0x0202, nullptr);
    const auto legacy = startup(0x0101, guarded.data() + 3);
    check(null_output == 10014 && legacy == 10092 && !runtime.winsock().references() && get() == 71 &&
              std::all_of(guarded.begin(), guarded.end(), [](auto byte) { return byte == std::byte{0xcc}; }),
          "invalid output and unsupported legacy versions acquire no reference and leave output/error state "
          "intact");
    const auto initialized = startup(0x0202, guarded.data() + 3);
    WinsockData data;
    std::memcpy(&data, guarded.data() + 3, sizeof(data));
    check(
        initialized == 0 && runtime.winsock().references() == 1 && !other.winsock().references() &&
            data.version == 0x0202 && data.highest_version == 0x0202 && !data.vendor &&
            !data.maximum_sockets && !data.maximum_datagram && data.description.back() == 0 &&
            data.status.back() == 0 && guarded[2] == std::byte{0xcc} && guarded[411] == std::byte{0xcc} &&
            std::all_of(data.padding.begin(), data.padding.end(),
                        [](auto byte) { return byte == std::byte{0}; }),
        "startup initializes exactly 408 Windows x64 bytes, no vendor pointer and independent bounded state");
    WinsockData older;
    WinsockData newer;
    const auto older_status = startup(0x0102, &older);
    const auto newer_status = startup(0x0303, &newer);
    check(older_status == 0 && newer_status == 0 && older.version == 0x0102 && newer.version == 0x0202 &&
              runtime.winsock().references() == 3,
          "Winsock2 negotiation retains supported lower versions and caps newer requests at 2.2");
    const auto first_cleanup = cleanup();
    const auto second_cleanup = cleanup();
    const auto third_cleanup = cleanup();
    check(!first_cleanup && !second_cleanup && !third_cleanup && !runtime.winsock().references() &&
              get() == 71,
          "balanced cleanup releases every owned initialization reference without clearing last error");
    const auto extra_cleanup = cleanup();
    check(extra_cleanup == -1 && get() == 10093 && errno == EDOM && crt_error() == 61,
          "cleanup without initialization returns SOCKET_ERROR/WSANOTINITIALISED and preserves native/CRT "
          "errors");
    {
        guest::ThreadContext first;
        guest::ThreadContext second;
        auto first_scope = first.activate();
        set(-42);
        {
            auto second_scope = second.activate();
            const auto initial = get();
            set(81);
            check(initial == 0 && get() == 81, "each guest context owns its Winsock last-error state");
        }
        check(get() == -42 && get_last_error() == std::bit_cast<std::uint32_t>(std::int32_t{-42}),
              "Winsock and Win32 last-error APIs share the active guest thread's full 32-bit error value");
    }
    bool filled = true;
    for (unsigned index = 0; index < 4096; ++index) {
        const auto result = startup(0x0202, &data);
        filled = filled && !result;
    }
    const auto exhausted = startup(0x0202, &data);
    check(filled && exhausted == 10091 && runtime.winsock().references() == 4096,
          "Winsock initialization references stop at the documented resource budget");
    bool released = true;
    for (unsigned index = 0; index < 4096; ++index) {
        const auto result = cleanup();
        released = released && !result;
    }
    check(released && !runtime.winsock().references(),
          "balanced cleanup recovers all initialization capacity");

    auto caller = fixture();
    text(caller, 0x440, "ws2_32.dll");
    put<std::uint64_t>(caller, 0x460, 0x8000000000000073ull);
    put<std::uint32_t>(caller, 0x1b8, 0x400); // More original zero-filled RW fixture space.
    constexpr unsigned char code[] = {0x48, 0x83, 0xec, 0x28, 0xb9, 2, 2, 0, 0,    0x48, 0x8d, 0x15, 0,   0,
                                      0,    0,    0xff, 0x15, 0,    0, 0, 0, 0x48, 0x83, 0xc4, 0x28, 0xc3};
    std::memcpy(caller.data() + 0x200, code, sizeof(code));
    put<std::uint32_t>(caller, 0x20c, 0x2140 - (0x1000 + 16));
    put<std::uint32_t>(caller, 0x212, 0x2080 - (0x1000 + 22));
    pe::ModuleGraph graph([&](const auto &import) { return runtime.resolve(import); });
    graph.add("startup.exe", caller);
    graph.load("startup.exe");
    Scope graph_scope(runtime, &graph);
    auto execution = graph.start();
    const auto result = execution.run_bootstrap();
    std::memcpy(&data, reinterpret_cast<const std::byte *>(graph.loaded_module()) + 0x2140, sizeof(data));
    check(result == 0 && data.version == 0x0202 && runtime.winsock().references() == 1,
          "an original PE performs real ordinal 115 startup and receives the complete Windows x64 output "
          "layout");
    using GetProc = void *(AMBROSIA_GUEST_ABI *)(void *, const char *);
    const auto get_proc =
        reinterpret_cast<GetProc>(runtime.resolve(service_symbol("GetProcAddress", "kernel32.dll")));
    const auto module = runtime.native_modules().handle("ws2_32.dll");
    const auto dynamic = get_proc(reinterpret_cast<void *>(module), reinterpret_cast<const char *>(115));
    check(reinterpret_cast<std::uintptr_t>(dynamic) == named &&
              !get_proc(reinterpret_cast<void *>(module), reinterpret_cast<const char *>(114)) &&
              get_last_error() == 127,
          "dynamic native ordinal lookup matches name/static identity and rejects sparse holes with "
          "ERROR_PROC_NOT_FOUND");
    (void)cleanup();
    errno = saved_error;
}
