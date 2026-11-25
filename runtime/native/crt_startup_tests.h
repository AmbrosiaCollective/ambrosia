void crt_startup_checks() {
    using namespace ambrosia::services;
    CrtStartup state;
    check(state.app_type() == 0 && *state.argument_count() == 0 && !*state.argument_vector(),
          "CRT startup begins with unknown app type and unconfigured arguments");
    check(state.set_app_type(2) && state.app_type() == 2 && !state.set_app_type(3) && state.app_type() == 2,
          "application type stores valid state and preserves it on invalid input");
    check(state.configure_arguments(1) == 22 && state.configure_arguments(0) == 0,
          "argument configuration requires the real image unless no arguments were requested");
    const std::filesystem::path image(u8"/work/tmp/fixture dir/guest-λ.exe");
    const auto image_utf8 = image.u8string();
    const std::string expected(reinterpret_cast<const char *>(image_utf8.data()), image_utf8.size());
    state.bind_image(image);
    check(state.configure_arguments(1) == 0 && *state.argument_count() == 1 && *state.argument_vector() &&
              std::string_view((*state.argument_vector())[0]) == expected && !(*state.argument_vector())[1],
          "narrow argv owns the dynamic UTF-8 executable path and a null terminator");
    auto *saved = (*state.argument_vector())[0];
    check(state.configure_arguments(2) == 0 && (*state.argument_vector())[0] == saved &&
              *state.argument_count() == 1,
          "expanded mode preserves argv zero when the CLI forwards no game arguments");
    check(state.configure_arguments(9) == 22 && (*state.argument_vector())[0] == saved,
          "invalid argv mode leaves configured argument storage unchanged");
    check(runtime_rejected([&] { state.bind_image("different.exe"); }) && std::string_view(saved) == expected,
          "one-shot image binding preserves already borrowed argument pointers");
    check(state.configure_arguments(0) == 0 && *state.argument_count() == 0 && !*state.argument_vector(),
          "no-argument mode clears the exported argc and argv state");
    check(*state.file_mode() == 0x4000 && state.set_file_mode(0x8000) == 0 && *state.file_mode() == 0x8000 &&
              state.set_file_mode(123) == 22 && *state.file_mode() == 0x8000,
          "CRT text/binary mode has owned state and rejects unsupported modes");
    check(!*state.environment() && state.initialize_environment() == 0 && *state.environment() &&
              !(*state.environment())[0],
          "guest environment initialization publishes an owned empty terminated array");
    auto **initial = state.initial_environment();
    auto **current = *state.environment();
    check(initial && !initial[0] && initial != current && state.initialize_environment() == 0 &&
              *state.environment() == current && state.initial_environment() == initial,
          "initial and current empty environments retain independent stable storage");
    check(*state.program_name() == saved && state.winmain_arguments()[0] == '\0',
          "startup exposes the real executable name and an empty no-forwarded-arguments tail");

    Runtime runtime;
    runtime.startup().bind_image(image);
    Scope scope(runtime);
    using AppType = void(AMBROSIA_GUEST_ABI *)(std::int32_t);
    using Configure = std::int32_t(AMBROSIA_GUEST_ABI *)(std::int32_t);
    using GetInt = std::int32_t *(AMBROSIA_GUEST_ABI *)();
    using GetArgv = char ***(AMBROSIA_GUEST_ABI *)();
    using Initialize = std::int32_t(AMBROSIA_GUEST_ABI *)();
    using GetEnvironment = char **(AMBROSIA_GUEST_ABI *)();
    const auto app = reinterpret_cast<AppType>(
        runtime.resolve(service_symbol("_set_app_type", "api-ms-win-crt-runtime-l1-1-0.dll")));
    const auto configure = reinterpret_cast<Configure>(
        runtime.resolve(service_symbol("_configure_narrow_argv", "ucrtbase.dll")));
    const auto argc = reinterpret_cast<GetInt>(runtime.resolve(service_symbol("__p___argc", "ucrtbase.dll")));
    const auto argv =
        reinterpret_cast<GetArgv>(runtime.resolve(service_symbol("__p___argv", "ucrtbase.dll")));
    const auto set_mode = reinterpret_cast<Configure>(
        runtime.resolve(service_symbol("_set_fmode", "api-ms-win-crt-stdio-l1-1-0.dll")));
    const auto get_mode =
        reinterpret_cast<GetInt>(runtime.resolve(service_symbol("__p__fmode", "ucrtbase.dll")));
    const auto environment = reinterpret_cast<GetArgv>(
        runtime.resolve(service_symbol("__p__environ", "api-ms-win-crt-environment-l1-1-0.dll")));
    const auto initialize = reinterpret_cast<Initialize>(
        runtime.resolve(service_symbol("_initialize_narrow_environment", "ucrtbase.dll")));
    const auto initial_environment = reinterpret_cast<GetEnvironment>(
        runtime.resolve(service_symbol("_get_initial_narrow_environment", "ucrtbase.dll")));
    set_last_error(51);
    crt_error() = 27;
    app(1);
    check(runtime.startup().app_type() == 1 && configure(1) == 0 && *argc() == 1 &&
              std::string_view((*argv())[0]) == expected && !(*argv())[1],
          "CRT startup Microsoft ABI changes application/argv state instead of returning a stub result");
    check(set_mode(0x8000) == 0 && *get_mode() == 0x8000 && get_last_error() == 51 && crt_error() == 27,
          "CRT file mode ABI updates owned mode while preserving success error state");
    check(set_mode(-1) == 22 && *get_mode() == 0x8000 && crt_error() == 22 && get_last_error() == 51,
          "invalid CRT file mode reports EINVAL without changing the existing mode");
    check(initialize() == 0 && *environment() && !(*environment())[0] && initial_environment() &&
              !initial_environment()[0],
          "CRT environment ABI exposes the guest's explicit empty initial/current environment");
    Runtime independent;
    {
        Scope nested(independent);
        app(2);
        check(independent.startup().app_type() == 2 && runtime.startup().app_type() == 1,
              "independent runtime does not overwrite another runtime's application type");
    }
    check(*argc() == 1 && std::string_view((*argv())[0]) == expected,
          "nested runtime restores the original argument bindings");
}

void startup_info_checks() {
    using namespace ambrosia;
    using namespace services;
    CrtStartup launch;
    check(launch.launch_info().size == 104 && !launch.launch_info().flags &&
              launch.show_window_mode() == 10 && !launch.launch_info().desktop &&
              !launch.launch_info().title && !launch.launch_info().standard_input &&
              !launch.launch_info().standard_output && !launch.launch_info().standard_error,
          "default owned launch metadata requests default show behavior without native desktop strings or "
          "inherited handles");
    check(launch.set_show_window(0) && launch.launch_info().flags == 1 && launch.show_window_mode() == 0 &&
              launch.set_show_window(11) && launch.show_window_mode() == 11 && !launch.set_show_window(12) &&
              launch.show_window_mode() == 11 && launch.set_show_window(std::nullopt) &&
              launch.show_window_mode() == 10,
          "explicit launch show modes set the flag, reject unsupported values transactionally and may be "
          "cleared");
    launch.set_show_window(3);
    launch.bind_image("app.exe");
    check(!launch.set_show_window(1) && launch.show_window_mode() == 3,
          "binding the executable freezes its original launch settings");
    Runtime runtime;
    Runtime independent;
    runtime.startup().set_show_window(3);
    using GetInfo = void(AMBROSIA_GUEST_ABI *)(void *);
    const auto address = runtime.resolve(service_symbol("GetStartupInfoW", "kernel32.dll"));
    const auto get = reinterpret_cast<GetInfo>(address);
    check(address && address == runtime.resolve(service_symbol("GetStartupInfoW", "kernelbase.dll")) &&
              address == runtime.resolve(
                             service_symbol("GetStartupInfoW", "api-ms-win-core-processthreads-l1-1-0.dll")),
          "startup metadata aliases share the explicit Windows ABI facade");
    Scope scope(runtime);
    const auto saved_error = errno;
    errno = E2BIG;
    crt_error() = 19;
    set_last_error(87);
    std::array<std::byte, 108> output;
    output.fill(std::byte{0xcc});
    get(output.data() + 2);
    const auto read32 = [&](std::size_t offset) {
        std::uint32_t value;
        std::memcpy(&value, output.data() + 2 + offset, sizeof(value));
        return value;
    };
    check(read32(0) == 104 && read32(60) == 1 && read32(64) == 3 &&
              runtime.startup().show_window_mode() == 3 && output[0] == std::byte{0xcc} &&
              output[1] == std::byte{0xcc} && output[106] == std::byte{0xcc} &&
              output[107] == std::byte{0xcc},
          "startup info writes exactly 104 bytes at the Windows x64 flag/show offsets without alignment "
          "assumptions");
    bool initialized = true;
    for (std::size_t i = 0; i < 104; ++i) {
        if (i != 0 && i != 60 && i != 64) {
            initialized &= output[i + 2] == std::byte{};
        }
    }
    check(initialized,
          "all reserved, pointer, handle and alignment bytes in the launch record are initialized");
    output[62] = std::byte{};
    get(output.data() + 2);
    check(read32(60) == 1 && independent.startup().show_window_mode() == 10,
          "guest output edits do not mutate owned launch settings or another runtime");
    check(errno == E2BIG && crt_error() == 19 && get_last_error() == 87,
          "startup metadata queries preserve native errno and guest error state");
    auto caller = fixture();
    text(caller, 0x440, "kernel32.dll");
    text(caller, 0x4a2, "GetStartupInfoW");
    constexpr unsigned char code[] = {0x48, 0x83, 0xec, 0x28, 0x48, 0x8d, 0x0d, 0,    0,    0,
                                      0,    0xff, 0x15, 0,    0,    0,    0,    0x8b, 0x05, 0,
                                      0,    0,    0,    0x48, 0x83, 0xc4, 0x28, 0xc3};
    std::memcpy(caller.data() + 0x200, code, sizeof(code));
    put<std::uint32_t>(caller, 0x207, 0x2110 - (0x1000 + 11));
    put<std::uint32_t>(caller, 0x20d, 0x2080 - (0x1000 + 17));
    put<std::uint32_t>(caller, 0x213, 0x2150 - (0x1000 + 23));
    auto image = pe::Image::load(caller, [&](const auto &symbol) { return runtime.resolve(symbol); });
    check(
        image.run_bootstrap() == 3,
        "an original PE reads configured show-window metadata from its actual Windows-layout startup buffer");
    errno = saved_error;
}
