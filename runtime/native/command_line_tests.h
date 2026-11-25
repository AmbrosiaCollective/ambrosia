void command_line_checks() {
    using namespace ambrosia;
    using namespace services;
    CrtStartup state;
    check(!state.command_line_a() && !state.command_line_w(),
          "unbound startup state has no invented host or empty process command line");
    const std::filesystem::path image(u8"Game data/沙滩/🌴.exe");
    const auto bytes = image.u8string();
    const std::string image_text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    const auto expected = std::string("\"") + image_text + "\"";
    const auto expected_wide = std::u16string(u"\"") + image.u16string() + u"\"";
    state.bind_image(image);
    auto *narrow = state.command_line_a();
    auto *wide = state.command_line_w();
    check(std::string_view(narrow) == expected && std::u16string_view(wide) == expected_wide &&
              expected_wide.find(u"\U0001f334") != expected_wide.npos,
          "command lines quote the selected image and preserve UTF-8/UTF-16 non-BMP names");
    state.configure_arguments(1);
    state.initialize_environment();
    state.set_file_mode(0x8000);
    check(std::string_view((*state.argument_vector())[0]) == image_text &&
              state.winmain_arguments()[0] == 0 && state.command_line_a() == narrow &&
              state.command_line_w() == wide,
          "CRT argv zero and empty WinMain tail remain consistent without moving command-line storage");
    *state.program_name() = nullptr;
    state.configure_arguments(0);
    check(std::string_view(narrow) == expected && std::u16string_view(wide) == expected_wide &&
              runtime_rejected([&] { state.bind_image("different.exe"); }),
          "CRT pointer/configuration changes and duplicate binding cannot replace the original process "
          "command line");
    for (const std::string &invalid : {std::string("a\0b.exe", 7), std::string("bad\"name.exe"),
                                       std::string(32765, 'a'), std::string("\xff.exe", 5)}) {
        CrtStartup rejected;
        check(runtime_rejected([&] { rejected.bind_image(std::filesystem::path(invalid)); }) &&
                  !rejected.command_line_a() && !rejected.command_line_w() && !*rejected.program_name(),
              "unrepresentable or oversized image names fail before publishing partial startup state");
        rejected.bind_image("valid.exe");
        check(std::string_view(rejected.command_line_a()) == "\"valid.exe\"",
              "a failed image conversion leaves startup state bindable");
    }
    CrtStartup largest;
    largest.bind_image(std::filesystem::path(std::string(32764, 'a')));
    check(std::char_traits<char16_t>::length(largest.command_line_w()) + 1 == 32767,
          "the UTF-16 command-line boundary includes both quotes and the NUL terminator");

    Runtime runtime;
    runtime.startup().bind_image(image);
    Runtime other;
    other.startup().bind_image("another.exe");
    Scope scope(runtime);
    using Narrow = char *(AMBROSIA_GUEST_ABI *)();
    using Wide = char16_t *(AMBROSIA_GUEST_ABI *)();
    const auto address =
        runtime.resolve(service_symbol("GetCommandLineA", "api-ms-win-core-processenvironment-l1-1-0.dll"));
    const auto get_narrow = reinterpret_cast<Narrow>(address);
    const auto get_wide =
        reinterpret_cast<Wide>(runtime.resolve(service_symbol("GetCommandLineW", "kernelbase.dll")));
    check(address && address == runtime.resolve(service_symbol("GetCommandLineA", "kernel32.dll")),
          "process command-line aliases share the native facade");
    set_last_error(83);
    crt_error() = 17;
    check(std::string_view(get_narrow()) == expected && std::u16string_view(get_wide()) == expected_wide &&
              get_narrow() == runtime.startup().command_line_a() && get_last_error() == 83 &&
              crt_error() == 17,
          "command-line Microsoft ABI returns stable owned strings and preserves successful error state");
    {
        Scope nested(other);
        check(std::string_view(get_narrow()) == "\"another.exe\"",
              "another guest process owns an independent command line");
    }
    check(std::string_view(get_narrow()) == expected,
          "nested scopes restore the original process command line");
    auto caller = fixture();
    text(caller, 0x440, "kernel32.dll");
    text(caller, 0x4a2, "GetCommandLineA");
    constexpr unsigned char code[] = {0x48, 0x83, 0xec, 0x28, 0xff, 0x15, 0,    0,    0,
                                      0,    0x0f, 0xb6, 0,    0x48, 0x83, 0xc4, 0x28, 0xc3};
    std::memcpy(caller.data() + 0x200, code, sizeof(code));
    put<std::uint32_t>(caller, 0x206, 0x2080 - (0x1000 + 10));
    auto executable = pe::Image::load(caller, [&](const auto &symbol) { return runtime.resolve(symbol); });
    check(
        executable.run_bootstrap() == '"',
        "an original PE import receives the bound quoted command line rather than a fabricated empty string");
}
