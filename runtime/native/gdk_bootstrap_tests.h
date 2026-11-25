void gdk_bootstrap_checks() {
    using namespace ambrosia;
    using namespace services;
    GdkBootstrap state;
    check(state.snapshot().references == 0 && state.initialize(0, 1) == gdk_invalid_argument &&
              state.snapshot().versions == 0,
          "GDK bootstrap starts uninitialized and invalid SDK input cannot publish a partial context");
    check(state.initialize(1, 2) == 0 && state.initialize(1, 2) == 0 && state.initialize(3, 4) == 0 &&
              state.snapshot().references == 3 && state.snapshot().versions == 2,
          "GDK bootstrap owns repeated references while tracking distinct requested SDK revisions");
    state.uninitialize();
    check(state.snapshot().references == 2 && state.snapshot().versions == 2,
          "partial GDK uninitialization retains the live context and observed revision set");
    state.uninitialize();
    state.uninitialize();
    state.uninitialize();
    check(state.snapshot().references == 0 && state.snapshot().versions == 0,
          "final GDK uninitialization clears session revisions without underflow on repeated cleanup");
    bool full = true;
    for (unsigned index = 0; index < 4096; ++index) {
        full &= state.initialize(1, 2) == 0;
    }
    check(full && state.initialize(1, 2) == gdk_out_of_memory && state.snapshot().references == 4096,
          "GDK reference accounting has a recoverable bounded process budget");
    for (unsigned index = 0; index < 4096; ++index) {
        state.uninitialize();
    }
    for (unsigned index = 1; index <= 64; ++index) {
        (void)state.initialize(index, 0);
    }
    check(state.initialize(65, 0) == gdk_out_of_memory && state.snapshot().versions == 64 &&
              state.snapshot().references == 64 && state.initialize(1, 0) == 0,
          "distinct GDK revision storage is bounded without rejecting a repeated supported session record");
    check(state.report_error(-7, "synthetic error") == 0 && state.snapshot().error_reports == 1 &&
              state.snapshot().last_error == -7 && state.last_error_message() == "synthetic error",
          "GDK error reporting stores owned code/text rather than returning an empty success");
    check(state.report_error(9, std::string(32769, 'x')) == gdk_invalid_argument &&
              state.snapshot().last_error == -7 && state.snapshot().error_reports == 1,
          "oversized GDK error reports fail transactionally without overwriting the previous report");
    Runtime runtime;
    Runtime independent;
    pe::ModuleGraph graph(resolve);
    graph.add("app.exe", fixture());
    graph.load("app.exe");
    Scope scope(runtime, &graph);
    using Load = void *(AMBROSIA_GUEST_ABI *)(const char16_t *, void *, std::uint32_t);
    using Lookup = void *(AMBROSIA_GUEST_ABI *)(void *, const char *);
    using Init = std::int32_t(AMBROSIA_GUEST_ABI *)(std::uint32_t, std::uint32_t);
    using InitEx = std::int32_t(AMBROSIA_GUEST_ABI *)(std::uint32_t, std::uint32_t, std::uint8_t);
    using InitEx2 =
        std::int32_t(AMBROSIA_GUEST_ABI *)(std::uint32_t, std::uint32_t, std::uint8_t, const void *);
    using Uninit = std::int32_t(AMBROSIA_GUEST_ABI *)();
    using Report = std::int32_t(AMBROSIA_GUEST_ABI *)(std::int32_t, const char *);
    using Free = std::int32_t(AMBROSIA_GUEST_ABI *)(void *);
    const auto load =
        reinterpret_cast<Load>(runtime.resolve(service_symbol("LoadLibraryExW", "kernel32.dll")));
    const auto lookup =
        reinterpret_cast<Lookup>(runtime.resolve(service_symbol("GetProcAddress", "kernel32.dll")));
    const auto release =
        reinterpret_cast<Free>(runtime.resolve(service_symbol("FreeLibrary", "kernel32.dll")));
    auto *module = load(u"xgameruntime.dll", nullptr, 0x800);
    check(module && runtime.library_references().count(reinterpret_cast<std::uintptr_t>(module)) == 1,
          "native GDK bootstrap exports are reachable through a counted owned PE service facade");
    const auto init = reinterpret_cast<Init>(lookup(module, "InitializeApiImpl"));
    const auto init_ex = reinterpret_cast<InitEx>(lookup(module, "InitializeApiImplEx"));
    const auto init_ex2 = reinterpret_cast<InitEx2>(lookup(module, "InitializeApiImplEx2"));
    const auto uninit = reinterpret_cast<Uninit>(lookup(module, "UninitializeApiImpl"));
    const auto report = reinterpret_cast<Report>(lookup(module, "XErrorReport"));
    check(init && init_ex && init_ex2 && uninit && report && lookup(module, "QueryApiImpl") &&
              reinterpret_cast<std::uintptr_t>(init_ex2) ==
                  runtime.resolve(service_symbol("InitializeApiImplEx2", "xgameruntime.dll")),
          "dynamic and static GDK bootstrap exports share exact typed canonical entrypoints");
    const auto saved_error = errno;
    errno = E2BIG;
    crt_error() = 19;
    set_last_error(87);
    check(init(1, 2) == 0 && init_ex(1, 2, 2) == 0 && init_ex2(3, 4, 2, nullptr) == 0 &&
              runtime.gdk_bootstrap().snapshot().references == 3 &&
              runtime.gdk_bootstrap().snapshot().versions == 2 &&
              independent.gdk_bootstrap().snapshot().references == 0,
          "all three GDK initialization ABIs bind owned state independently for each native runtime");
    char message[] = "owned report";
    check(report(-11, message) == 0 && runtime.gdk_bootstrap().snapshot().last_error == -11 &&
              runtime.gdk_bootstrap().snapshot().error_reports == 1,
          "dynamic XErrorReport retains its actual report in the scoped runtime");
    message[0] = 'x';
    check(runtime.gdk_bootstrap().last_error_message() == "owned report" && report(0, nullptr) == 0 &&
              runtime.gdk_bootstrap().last_error_message().empty(),
          "error report ownership is independent of caller buffers and null messages are empty");
    check(uninit() == 0 && runtime.gdk_bootstrap().snapshot().references == 2 && uninit() == 0 &&
              uninit() == 0 && uninit() == 0 && runtime.gdk_bootstrap().snapshot().references == 0,
          "dynamic GDK cleanup balances initialization and tolerates an already-cleared context");
    check(errno == E2BIG && crt_error() == 19 && get_last_error() == 87,
          "successful GDK initialization, reports and cleanup preserve native and guest error state");
    auto caller = fixture();
    text(caller, 0x440, "xgameruntime.dll");
    text(caller, 0x4a2, "InitializeApiImplEx2");
    constexpr unsigned char code[] = {0x48, 0x83, 0xec, 0x28, 0xb9, 1,    0,    0,    0,    0xba, 2,    0,
                                      0,    0,    0x41, 0xb8, 2,    0,    0,    0,    0x45, 0x31, 0xc9, 0xff,
                                      0x15, 0,    0,    0,    0,    0x48, 0x83, 0xc4, 0x28, 0xc3};
    std::memcpy(caller.data() + 0x200, code, sizeof(code));
    put<std::uint32_t>(caller, 0x219, 0x2080 - (0x1000 + 29));
    auto image = pe::Image::load(caller, [&](const auto &symbol) { return runtime.resolve(symbol); });
    check(image.run_bootstrap() == 0 && runtime.gdk_bootstrap().snapshot().references == 1,
          "an original PE passes four Windows register arguments and initializes the real owned GDK "
          "bootstrap context");
    uninit();
    check(release(module) &&
              runtime.library_references().count(reinterpret_cast<std::uintptr_t>(module)) == 0,
          "GDK initialization references and dynamic-library references have distinct balanced lifetimes");
    errno = saved_error;
}
