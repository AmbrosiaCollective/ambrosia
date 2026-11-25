std::vector<unsigned> exit_events;
ambrosia::services::ExitCallbacks *reentrant_exit = nullptr;

void AMBROSIA_GUEST_ABI exit_first() {
    exit_events.push_back(1);
}

void AMBROSIA_GUEST_ABI exit_second() {
    exit_events.push_back(2);
}

void AMBROSIA_GUEST_ABI exit_register_later() {
    exit_events.push_back(3);
    if (reentrant_exit->add(&exit_first)) {
        throw std::runtime_error("synthetic exit registration failed");
    }
}

std::string exit_order;
bool exit_context_valid = true;

void AMBROSIA_GUEST_ABI exit_with_tls() {
    auto *tls = current_guest_tls();
    exit_context_valid = exit_context_valid && ambrosia::guest::ThreadContext::current() && tls[0] == 42;
    tls[0] = 43;
    exit_order += 'C';
}

void AMBROSIA_GUEST_ABI exit_tls_notify(void *, std::uint32_t reason, void *) {
    auto *tls = current_guest_tls();
    exit_context_valid = exit_context_valid && tls[0] == (reason == 1 ? 41u : 43u);
    exit_order += reason == 1 ? 'A' : 'D';
}

std::uint64_t AMBROSIA_GUEST_ABI exit_registering_entry() {
    exit_order += 'E';
    current_guest_tls()[0] = 42;
    using Register = std::int32_t(AMBROSIA_GUEST_ABI *)(ambrosia::services::Initializer);
    auto *runtime = ambrosia::services::current_runtime();
    const auto add = reinterpret_cast<Register>(
        runtime->resolve(service_symbol("_crt_atexit", "api-ms-win-crt-runtime-l1-1-0.dll")));
    return add(&exit_with_tls) == 0 ? 42 : 0;
}

void crt_exit_checks() {
    using namespace ambrosia::services;
    ExitCallbacks callbacks;
    exit_events.clear();
    check(callbacks.add(nullptr) == -1 && callbacks.add(&exit_first) == 0 &&
              callbacks.add(&exit_second) == 0 && exit_events.empty(),
          "CRT exit registration owns valid callbacks without running them");
    callbacks.execute();
    check(exit_events == std::vector<unsigned>{2, 1},
          "process cleanup invokes void callbacks in reverse order");
    callbacks.execute();
    check(exit_events.size() == 2, "completed CRT cleanup does not invoke callbacks again");
    reentrant_exit = &callbacks;
    callbacks.add(&exit_register_later);
    exit_events.clear();
    callbacks.execute();
    check(exit_events == std::vector<unsigned>{3},
          "callback-time registration forms a new owned cleanup batch");
    callbacks.execute();
    check(exit_events == std::vector<unsigned>{3, 1}, "later explicit cleanup invokes the new batch");
    ExitCallbacks bounded;
    bool filled = true;
    for (unsigned index = 0; index < 4096; ++index) {
        filled = filled && bounded.add(&exit_first) == 0;
    }
    check(filled && bounded.add(&exit_first) == -1, "CRT process cleanup has a bounded callback registry");
    bounded.execute();

    Runtime runtime;
    using Register = std::int32_t(AMBROSIA_GUEST_ABI *)(Initializer);
    const auto add = reinterpret_cast<Register>(
        runtime.resolve(service_symbol("_crt_atexit", "api-ms-win-crt-runtime-l1-1-0.dll")));
    check(add(&exit_first) == -1, "CRT exit service requires an active runtime");
    {
        Scope scope(runtime);
        exit_events.clear();
        set_last_error(52);
        crt_error() = 28;
        check(add(&exit_first) == 0 && add(&exit_second) == 0 && exit_events.empty(),
              "Microsoft ABI exit registration retains correctly typed void callbacks");
        runtime.exit_callbacks().execute();
        check(exit_events == std::vector<unsigned>{2, 1} && get_last_error() == 52 && crt_error() == 28,
              "exit callbacks execute with preserved successful service error state");
    }

    auto bytes = tls_fixture();
    put<std::uint32_t>(bytes, 0x98 + 120, 0);
    put<std::uint32_t>(bytes, 0x98 + 124, 0);
    jump_to_host(bytes, 0x200, reinterpret_cast<std::uintptr_t>(&exit_registering_entry));
    jump_to_host(bytes, 0x300, reinterpret_cast<std::uintptr_t>(&exit_tls_notify));
    const std::filesystem::path path = "/work/tmp/crt-exit-fixture.exe";
    {
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file) {
            throw std::runtime_error("synthetic cleanup fixture write failed");
        }
    }
    const auto original_gs = ambrosia::host::guest_segment_base();
    exit_order.clear();
    exit_context_valid = true;
    const std::array<std::filesystem::path, 2> arguments{"--run-bootstrap", path};
    std::ostringstream output, errors;
    check(ambrosia::run_native_command(arguments, output, errors) == 0 && errors.str().empty() &&
              output.str().find("\"bootstrap_result\":42") != std::string::npos,
          "native CLI bootstrap executes real registered CRT cleanup before returning");
    check(exit_order == "AECD" && exit_context_valid,
          "CRT cleanup retains guest TLS and precedes module TLS process detach");
    check(ambrosia::host::guest_segment_base() == original_gs,
          "CLI cleanup restores native GS after guest callbacks and detach");

    const std::array<std::filesystem::path, 2> trace_arguments{"--trace-startup", path};
    std::ostringstream trace_output, trace_errors;
    exit_order.clear();
    exit_context_valid = true;
    const auto trace_code = ambrosia::run_native_command(trace_arguments, trace_output, trace_errors);
    const auto messages = trace_errors.str();
    const auto module_stage = messages.find("TRACE stage=module-initialization");
    const auto entry_stage = messages.find("TRACE stage=executable-entry");
    const auto cleanup_stage = messages.find("TRACE stage=returned-entry-cleanup");
    check(trace_code == 3 && module_stage != std::string::npos && entry_stage != std::string::npos &&
              cleanup_stage != std::string::npos && module_stage < entry_stage &&
              entry_stage < cleanup_stage && exit_order == "AECD" && exit_context_valid,
          "diagnostic stage evidence matches actual PE entry and guest cleanup lifecycle order");

    ambrosia::pe::ModuleGraph graph(resolve);
    graph.add("app.exe", fixture());
    graph.load("app.exe");
    auto execution = graph.start();
    check(runtime_rejected([&] { execution.with_guest_context({}); }),
          "guest context invocation rejects a missing callback");
    execution.close();
    check(runtime_rejected([&] { execution.with_guest_context([] {}); }),
          "closed module execution cannot invoke guest cleanup");
}

std::string executable_exit_order;
bool executable_exit_abi = true;
ambrosia::services::ExitCallbacks *tls_registering_exit = nullptr;

void AMBROSIA_GUEST_ABI executable_tls_exit(void *module, std::uint32_t reason, void *reserved) {
    executable_exit_abi &= !module && reason == 0 && !reserved;
    executable_exit_order += 'T';
    if (tls_registering_exit && tls_registering_exit->add(&exit_first)) {
        throw std::runtime_error("TLS cleanup synthetic exit registration failed");
    }
}

void AMBROSIA_GUEST_ABI replaced_tls_exit(void *, std::uint32_t, void *) {
    executable_exit_order += 'R';
}

void AMBROSIA_GUEST_ABI executable_tls_with_context(void *module, std::uint32_t reason, void *reserved) {
    executable_exit_abi &= !module && reason == 0 && !reserved;
    auto *tls = current_guest_tls();
    exit_context_valid &= ambrosia::guest::ThreadContext::current() && tls[0] == 42;
    exit_order += 'T';
}

std::uint64_t AMBROSIA_GUEST_ABI register_executable_tls_entry() {
    const auto result = exit_registering_entry();
    using Register = void(AMBROSIA_GUEST_ABI *)(ambrosia::services::TlsExitCallback);
    auto *runtime = ambrosia::services::current_runtime();
    const auto add = reinterpret_cast<Register>(runtime->resolve(
        service_symbol("_register_thread_local_exe_atexit_callback", "api-ms-win-crt-runtime-l1-1-0.dll")));
    add(&executable_tls_with_context);
    return result;
}

void executable_tls_exit_checks() {
    using namespace ambrosia;
    using namespace services;
    ExitCallbacks callbacks;
    executable_exit_order.clear();
    executable_exit_abi = true;
    callbacks.set_tls(&replaced_tls_exit);
    callbacks.set_tls(&executable_tls_exit);
    check(executable_exit_order.empty(),
          "executable TLS exit registration replaces its slot without executing guest code");
    callbacks.execute();
    callbacks.execute();
    check(executable_exit_order == "T" && executable_exit_abi,
          "executable TLS cleanup uses three correct arguments and consumes its owned callback once");
    callbacks.set_tls(&executable_tls_exit);
    callbacks.set_tls(nullptr);
    callbacks.execute();
    check(executable_exit_order == "T",
          "a null executable TLS exit registration clears the previous callback");
    callbacks.add(&exit_second);
    callbacks.set_tls(&executable_tls_exit);
    tls_registering_exit = &callbacks;
    exit_events.clear();
    callbacks.execute();
    tls_registering_exit = nullptr;
    check(
        executable_exit_order == "TT" && exit_events == std::vector<unsigned>{1, 2},
        "ordinary callbacks registered by executable TLS destructors join the same cleanup in reverse order");

    Runtime runtime;
    Scope scope(runtime);
    using Register = void(AMBROSIA_GUEST_ABI *)(TlsExitCallback);
    const auto address =
        runtime.resolve(service_symbol("_register_thread_local_exe_atexit_callback", "ucrtbase.dll"));
    const auto add = reinterpret_cast<Register>(address);
    check(address &&
              address == runtime.resolve(
                             service_symbol("_register_thread_local_exe_atexit_callback", "msvcrt.dll")) &&
              address == runtime.resolve(service_symbol("_register_thread_local_exe_atexit_callback",
                                                        "api-ms-win-crt-runtime-l1-1-0.dll")),
          "executable TLS registration aliases share the typed Microsoft ABI facade");
    const auto saved_error = errno;
    errno = E2BIG;
    crt_error() = 19;
    set_last_error(87);
    add(&executable_tls_exit);
    check(errno == E2BIG && crt_error() == 19 && get_last_error() == 87,
          "TLS cleanup registration preserves native errno and both guest error states");
    executable_exit_order.clear();
    runtime.exit_callbacks().execute();
    check(executable_exit_order == "T", "the service retains executable TLS cleanup in its active runtime");

    auto caller = fixture();
    text(caller, 0x440, "ucrtbase.dll");
    text(caller, 0x4a2, "_register_thread_local_exe_atexit_callback");
    // sub rsp,28h; RCX=null; call IAT; return constant 7 after the void setter.
    constexpr unsigned char code[] = {0x48, 0x83, 0xec, 0x28, 0x31, 0xc9, 0xff, 0x15, 0,    0,    0,
                                      0,    0xb8, 7,    0,    0,    0,    0x48, 0x83, 0xc4, 0x28, 0xc3};
    std::memcpy(caller.data() + 0x200, code, sizeof(code));
    put<std::uint32_t>(caller, 0x208, 0x2080 - (0x1000 + 12));
    auto image = pe::Image::load(caller, [&](const auto &symbol) { return runtime.resolve(symbol); });
    add(&replaced_tls_exit);
    executable_exit_order.clear();
    check(image.run_bootstrap() == 7,
          "an original PE passes a TLS callback pointer through the void registration import");
    runtime.exit_callbacks().execute();
    check(executable_exit_order.empty(),
          "the PE's null registration clears previously owned executable cleanup");

    auto bytes = tls_fixture();
    put<std::uint32_t>(bytes, 0x98 + 120, 0);
    put<std::uint32_t>(bytes, 0x98 + 124, 0);
    jump_to_host(bytes, 0x200, reinterpret_cast<std::uintptr_t>(&register_executable_tls_entry));
    jump_to_host(bytes, 0x300, reinterpret_cast<std::uintptr_t>(&exit_tls_notify));
    const std::filesystem::path path = "/work/tmp/executable-tls-exit.exe";
    {
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file) {
            throw std::runtime_error("synthetic executable TLS fixture write failed");
        }
    }
    exit_order.clear();
    exit_context_valid = true;
    executable_exit_abi = true;
    const auto original_gs = host::guest_segment_base();
    std::ostringstream output, errors;
    const std::array<std::filesystem::path, 2> arguments{"--run-bootstrap", path};
    check(run_native_command(arguments, output, errors) == 0 && errors.str().empty() &&
              exit_order == "AETCD" && executable_exit_abi && exit_context_valid &&
              host::guest_segment_base() == original_gs,
          "executable TLS cleanup retains guest context and precedes ordinary exit then module detach with "
          "GS restored");
    errno = saved_error;
}
