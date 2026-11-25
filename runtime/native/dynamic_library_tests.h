void dynamic_library_checks() {
    using namespace ambrosia;
    using namespace services;
    LibraryReferences references;
    check(!references.retain(0) && !references.release(7) && references.retain(7) && references.retain(7) &&
              references.count(7) == 2 && references.release(7) && references.count(7) == 1 &&
              references.release(7) && references.count(7) == 0 && !references.release(7),
          "dynamic library references balance independently of non-owning handles without underflow");
    bool full = true;
    for (unsigned i = 0; i < 4096; ++i) {
        full &= references.retain(7);
    }
    check(full && !references.retain(8) && references.release(7) && references.retain(8),
          "dynamic module reference totals are bounded and recover after release");
    LibraryReferences directory;
    for (unsigned i = 1; i <= 128; ++i) {
        (void)directory.retain(i);
    }
    check(!directory.retain(129) && directory.release(1) && directory.retain(129),
          "dynamic reference tracking has a bounded distinct-handle directory");
    Runtime runtime;
    Runtime independent;
    pe::ModuleGraph graph(resolve);
    graph.add("app.exe", fixture());
    graph.add("provider.dll", provider_fixture());
    graph.add("unmapped.dll", provider_fixture());
    graph.load("app.exe");
    (void)graph.export_address("provider.dll", named_symbol("named_value"));
    check(graph.has_available_source("unmapped.dll") && !graph.has_available_source("missing.dll") &&
              !graph.loaded_module("unmapped.dll"),
          "available package-source detection never maps or initializes a source-only module");
    Scope scope(runtime, &graph);
    using LoadW = void *(AMBROSIA_GUEST_ABI *)(const char16_t *);
    using LoadA = void *(AMBROSIA_GUEST_ABI *)(const char *);
    using LoadExW = void *(AMBROSIA_GUEST_ABI *)(const char16_t *, void *, std::uint32_t);
    using LoadExA = void *(AMBROSIA_GUEST_ABI *)(const char *, void *, std::uint32_t);
    using Free = std::int32_t(AMBROSIA_GUEST_ABI *)(void *);
    using Lookup = void *(AMBROSIA_GUEST_ABI *)(void *, const char *);
    const auto wide =
        reinterpret_cast<LoadW>(runtime.resolve(service_symbol("LoadLibraryW", "kernel32.dll")));
    const auto narrow =
        reinterpret_cast<LoadA>(runtime.resolve(service_symbol("LoadLibraryA", "kernel32.dll")));
    const auto ex_w =
        reinterpret_cast<LoadExW>(runtime.resolve(service_symbol("LoadLibraryExW", "kernel32.dll")));
    const auto ex_a =
        reinterpret_cast<LoadExA>(runtime.resolve(service_symbol("LoadLibraryExA", "kernel32.dll")));
    const auto release =
        reinterpret_cast<Free>(runtime.resolve(service_symbol("FreeLibrary", "kernel32.dll")));
    const auto lookup =
        reinterpret_cast<Lookup>(runtime.resolve(service_symbol("GetProcAddress", "kernel32.dll")));
    bool aliases = true;
    for (const auto *name :
         {"LoadLibraryW", "LoadLibraryA", "LoadLibraryExW", "LoadLibraryExA", "FreeLibrary"}) {
        const auto entry = runtime.resolve(service_symbol(name, "kernel32.dll"));
        aliases &= entry && entry == runtime.resolve(service_symbol(name, "kernelbase.dll")) &&
                   entry == runtime.resolve(service_symbol(name, "api-ms-win-core-libraryloader-l1-2-0.dll"));
    }
    check(aliases, "dynamic library facade APIs share callable kernel/API-set aliases");
    const auto saved_error = errno;
    errno = E2BIG;
    crt_error() = 19;
    set_last_error(87);
    auto *first = wide(u"KERNEL32");
    auto *second = ex_a("kernel32.dll   ", nullptr, 0x800);
    const auto base = reinterpret_cast<std::uintptr_t>(first);
    check(first && second == first && runtime.library_references().count(base) == 2 &&
              independent.library_references().count(base) == 0 &&
              reinterpret_cast<std::uintptr_t>(lookup(first, "QueryPerformanceCounter")) ==
                  runtime.resolve(service_symbol("QueryPerformanceCounter", "kernel32.dll")),
          "owned PE service facades retain counted dynamic references and expose the same real exports as "
          "static binding");
    check(release(first) && runtime.library_references().count(base) == 1 && release(second) &&
              runtime.library_references().count(base) == 0 &&
              runtime.native_modules().handle("kernel32.dll") == base,
          "balancing extra library references retains the process-pinned native catalog through cleanup");
    auto *package = ex_w(u"provider.dll", nullptr, 0x1000);
    const auto provider = graph.loaded_module("provider.dll");
    check(reinterpret_cast<std::uintptr_t>(package) == provider &&
              runtime.library_references().count(provider) == 1 && release(package) &&
              graph.loaded_module("provider.dll") == provider,
          "already initialized package modules accept extra references while their graph pin remains owned");
    auto *plain = narrow("kernel32.dll");
    check(plain == first && release(plain) && errno == E2BIG && crt_error() == 19 && get_last_error() == 87,
          "ordinary and Ex loads and balanced releases preserve native/guest errors on success");
    check(!wide(nullptr) && get_last_error() == 87 && !ex_a("kernel32.dll", reinterpret_cast<void *>(1), 0) &&
              get_last_error() == 87,
          "null library names and nonnull reserved-file handles fail without retaining a module");
    check(!narrow("missing.dll") && get_last_error() == 126 && !wide(u"") && get_last_error() == 126 &&
              !ex_w(u"unmapped.dll", nullptr, 0x800) && get_last_error() == 126,
          "absent modules and uninitialized application sources excluded by system-only search cannot "
          "trigger host loading");
    check(
        !release(first) && get_last_error() == 6 && !release(nullptr) && get_last_error() == 6,
        "FreeLibrary rejects missing extra references and null handles without releasing graph/catalog pins");

    auto caller = fixture();
    text(caller, 0x440, "kernel32.dll");
    text(caller, 0x4a2, "LoadLibraryExW");
    const char16_t name[] = u"kernel32.dll";
    std::memcpy(caller.data() + 0x540, name, sizeof(name));
    constexpr unsigned char code[] = {0x48, 0x83, 0xec, 0x28, 0x48, 0x8d, 0x0d, 0,    0,    0,
                                      0,    0x31, 0xd2, 0x41, 0xb8, 0,    8,    0,    0,    0xff,
                                      0x15, 0,    0,    0,    0,    0x48, 0x83, 0xc4, 0x28, 0xc3};
    std::memcpy(caller.data() + 0x200, code, sizeof(code));
    put<std::uint32_t>(caller, 0x207, 0x2140 - (0x1000 + 11));
    put<std::uint32_t>(caller, 0x215, 0x2080 - (0x1000 + 25));
    auto image = pe::Image::load(caller, [&](const auto &symbol) { return runtime.resolve(symbol); });
    check(image.run_bootstrap() == base && runtime.library_references().count(base) == 1 && release(first),
          "an original PE passes a UTF-16 module name and Ex search flags and receives a counted real facade "
          "handle");
    errno = saved_error;
}
