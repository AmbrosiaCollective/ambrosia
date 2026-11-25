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
