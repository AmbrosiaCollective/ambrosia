struct InvalidParameterRecord {
    unsigned calls = 0;
    const char16_t *expression = nullptr;
    const char16_t *function = nullptr;
    const char16_t *file = nullptr;
    std::uint32_t line = 0;
    std::uintptr_t reserved = 0;
    bool guest_context = true;
    std::uint32_t tls_value = 0;
};

InvalidParameterRecord *active_invalid_record = nullptr;
unsigned local_invalid_calls = 0;

void AMBROSIA_GUEST_ABI global_invalid_parameter(const char16_t *expression, const char16_t *function,
                                                 const char16_t *file, std::uint32_t line,
                                                 std::uintptr_t reserved) {
    auto &record = *active_invalid_record;
    ++record.calls;
    record.expression = expression;
    record.function = function;
    record.file = file;
    record.line = line;
    record.reserved = reserved;
    record.guest_context = record.guest_context && ambrosia::guest::ThreadContext::current();
    if (auto *graph = ambrosia::services::current_modules()) {
        record.tls_value = *graph_tls(reinterpret_cast<void *>(graph->loaded_module()));
    }
}

void AMBROSIA_GUEST_ABI local_invalid_parameter(const char16_t *, const char16_t *, const char16_t *,
                                                std::uint32_t, std::uintptr_t) {
    ++local_invalid_calls;
}

void invalid_parameter_checks() {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    Runtime independent;
    Scope scope(runtime);
    using Set = InvalidParameterHandler(AMBROSIA_GUEST_ABI *)(InvalidParameterHandler);
    using Get = InvalidParameterHandler(AMBROSIA_GUEST_ABI *)();
    using Dispatch = void(AMBROSIA_GUEST_ABI *)(const char16_t *, const char16_t *, const char16_t *,
                                                std::uint32_t, std::uintptr_t);
    using NoInfo = void(AMBROSIA_GUEST_ABI *)();
    const auto set = reinterpret_cast<Set>(
        runtime.resolve(service_symbol("_set_invalid_parameter_handler", "ucrtbase.dll")));
    const auto get = reinterpret_cast<Get>(
        runtime.resolve(service_symbol("_get_invalid_parameter_handler", "ucrtbase.dll")));
    const auto set_local = reinterpret_cast<Set>(
        runtime.resolve(service_symbol("_set_thread_local_invalid_parameter_handler", "ucrtbase.dll")));
    const auto get_local = reinterpret_cast<Get>(
        runtime.resolve(service_symbol("_get_thread_local_invalid_parameter_handler", "ucrtbase.dll")));
    const auto dispatch =
        reinterpret_cast<Dispatch>(runtime.resolve(service_symbol("_invalid_parameter", "ucrtbase.dll")));
    const auto noinfo = reinterpret_cast<NoInfo>(
        runtime.resolve(service_symbol("_invalid_parameter_noinfo", "ucrtbase.dll")));
    check(reinterpret_cast<std::uintptr_t>(set) ==
                  runtime.resolve(service_symbol("_set_invalid_parameter_handler",
                                                 "api-ms-win-crt-runtime-l1-1-0.dll")) &&
              !get(),
          "CRT invalid-parameter aliases share a real typed handler slot initially empty");
    const auto previous = set(&global_invalid_parameter);
    check(!previous && get() == &global_invalid_parameter && !independent.invalid_parameters().handler(),
          "global handler exchange returns its predecessor and isolates independent runtimes");
    InvalidParameterRecord record;
    active_invalid_record = &record;
    const auto saved_error = errno;
    errno = EDOM;
    guest::ThreadContext first;
    guest::ThreadContext second;
    {
        auto activation = first.activate();
        crt_error() = 91;
        set_last_error(37);
        const char16_t expression[] = u"expression";
        const char16_t function[] = u"function";
        const char16_t file[] = u"fixture.cpp";
        dispatch(expression, function, file, 0xfedcba98, 0x12345678abcdef01ull);
        check(
            record.calls == 1 && record.expression == expression && record.function == function &&
                record.file == file && record.line == 0xfedcba98 &&
                record.reserved == 0x12345678abcdef01ull && record.guest_context,
            "invalid-parameter dispatch preserves all five Windows callback arguments and active guest TLS");
        local_invalid_calls = 0;
        const auto local_previous = set_local(&local_invalid_parameter);
        noinfo();
        check(!local_previous && get_local() == &local_invalid_parameter && local_invalid_calls == 1 &&
                  record.calls == 1,
              "a guest-local handler takes precedence without overwriting the runtime-global handler");
        {
            auto nested = second.activate();
            const auto second_initial = get_local();
            noinfo();
            check(!second_initial && record.calls == 2 && !record.expression && !record.function &&
                      !record.file && !record.line && !record.reserved,
                  "another guest context starts with no local handler and noinfo forwards null metadata to "
                  "the global handler");
        }
        const auto restored = get_local();
        const auto removed = set_local(nullptr);
        noinfo();
        check(restored == &local_invalid_parameter && removed == restored && record.calls == 3 &&
                  !get_local(),
              "restoring the first guest context restores its local handler; clearing falls back to the "
              "global handler");
        check(errno == EDOM && crt_error() == 91 && get_last_error() == 37,
              "registration and dispatch preserve native errno and unmodified guest error channels");
        using NewMode = std::int32_t(AMBROSIA_GUEST_ABI *)(std::int32_t);
        const auto new_mode =
            reinterpret_cast<NewMode>(runtime.resolve(service_symbol("_set_new_mode", "ucrtbase.dll")));
        const auto invalid_mode = new_mode(9);
        check(invalid_mode == -1 && crt_error() == 22 && record.calls == 4 &&
                  runtime.new_allocations().mode() == 0,
              "invalid allocation mode invokes the registered handler then returns failure without mutating "
              "mode");
        using Wide = std::int32_t(AMBROSIA_GUEST_ABI *)(std::uint64_t, char16_t *, std::size_t,
                                                        const char16_t *, void *, const std::byte *);
        const auto wide = reinterpret_cast<Wide>(
            runtime.resolve(service_symbol("__stdio_common_vswprintf_s", "ucrtbase.dll")));
        std::array<char16_t, 6> output{u'L', u'x', u'x', u'x', u'x', u'R'};
        const auto range = wide(36, output.data() + 1, 3, u"abc", nullptr, nullptr);
        check(range == -1 && output[0] == u'L' && output[1] == 0 && output[4] == u'x' && output[5] == u'R' &&
                  crt_error() == 34 && record.calls == 5,
              "secure formatter range failure clears only the first unit, dispatches the handler and returns "
              "ERANGE/-1");
        const std::uint64_t null_argument = 0;
        const auto null_string = wide(36, output.data() + 1, 4, u"%s", nullptr,
                                      reinterpret_cast<const std::byte *>(&null_argument));
        const auto percent_n = wide(36, output.data() + 1, 4, u"%n", nullptr,
                                    reinterpret_cast<const std::byte *>(&null_argument));
        check(null_string == -1 && percent_n == -1 && crt_error() == 22 && record.calls == 7,
              "secure null-string and disabled percent-n validation invoke the real callback rather than "
              "returning fake success");
    }
    auto caller = tls_fixture();
    text(caller, 0x440, "ucrtbase.dll");
    text(caller, 0x4a2, "_invalid_parameter");
    jump_to_host(caller, 0x300, reinterpret_cast<std::uintptr_t>(&native_thread_tls));
    constexpr unsigned char code[] = {0x48, 0x83, 0xec, 0x38, 0x31, 0xc9, 0x31, 0xd2, 0x45, 0x31,
                                      0xc0, 0x41, 0xb9, 42,   0,    0,    0,    0x48, 0xc7, 0x44,
                                      0x24, 0x20, 99,   0,    0,    0,    0xff, 0x15, 0,    0,
                                      0,    0,    0x31, 0xc0, 0x48, 0x83, 0xc4, 0x38, 0xc3};
    std::memcpy(caller.data() + 0x200, code, sizeof(code));
    put<std::uint32_t>(caller, 0x21c, 0x2080 - (0x1000 + 32));
    pe::ModuleGraph graph([&](const auto &symbol) { return runtime.resolve(symbol); });
    graph.add("invalid.exe", caller);
    graph.load("invalid.exe");
    Scope graph_scope(runtime, &graph);
    auto process = graph.start();
    const auto returned = process.run_bootstrap();
    check(returned == 0 && record.calls == 8 && record.line == 42 && record.reserved == 99 &&
              record.tls_value == 41,
          "an original PE invokes the Windows five-argument handler path with its own static TLS and stack "
          "argument");
    const auto removed_global = set(nullptr);
    check(removed_global == &global_invalid_parameter && !get(),
          "global handler removal returns the previous pointer");
    active_invalid_record = nullptr;
    errno = saved_error;
}
