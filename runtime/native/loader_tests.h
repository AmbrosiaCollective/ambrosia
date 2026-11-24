std::vector<std::uint32_t> loader_reasons;
std::int32_t loader_disable_result = 0;
std::uint32_t loader_disable_error = 0;

std::int32_t AMBROSIA_GUEST_ABI loader_dll_entry(void *module, std::uint32_t reason, void *) {
    loader_reasons.push_back(reason);
    if (reason == 1) {
        using Disable = std::int32_t(AMBROSIA_GUEST_ABI *)(void *);
        auto *runtime = ambrosia::services::current_runtime();
        const auto disable = reinterpret_cast<Disable>(runtime->resolve(
            service_symbol("DisableThreadLibraryCalls", "api-ms-win-core-libraryloader-l1-2-0.dll")));
        ambrosia::services::set_last_error(49);
        loader_disable_result = disable(module);
        loader_disable_error = ambrosia::services::get_last_error();
    }
    return 1;
}

void loader_policy_checks() {
    using namespace ambrosia;
    services::Runtime runtime;
    pe::ModuleGraph graph(resolve);
    auto plain = provider_fixture();
    put<std::uint32_t>(plain, 0x98 + 16, 0x1000);
    jump_to_host(plain, 0x200, reinterpret_cast<std::uintptr_t>(&loader_dll_entry));
    graph.add("plain.dll", plain);
    graph.add("tls.dll", tls_fixture(true));
    graph.add("app.exe", fixture());
    graph.load("plain.dll");
    (void)graph.export_address("tls.dll", named_symbol("absent"));
    (void)graph.export_address("app.exe", named_symbol("absent"));
    auto base = [&](const std::string &name) {
        for (const auto &module : graph.diagnostic_ranges()) {
            if (module.name == name) {
                return module.base;
            }
        }
        throw std::runtime_error("synthetic loader module missing");
    };
    const auto plain_base = base("plain.dll");
    check(graph.thread_notifications_enabled(plain_base) == true,
          "loaded module initially permits thread notifications");
    check(!graph.thread_notifications_enabled(1).has_value(), "unknown module has no notification policy");
    using Disable = std::int32_t(AMBROSIA_GUEST_ABI *)(void *);
    const auto disable = reinterpret_cast<Disable>(runtime.resolve(
        service_symbol("DisableThreadLibraryCalls", "api-ms-win-core-libraryloader-l1-2-0.dll")));
    check(disable(reinterpret_cast<void *>(plain_base)) == 0 && services::get_last_error() == 126,
          "module service without scoped graph fails explicitly");
    {
        services::Scope scope(runtime, &graph);
        check(services::current_modules() == &graph, "service scope binds the current module graph");
        check(disable(nullptr) == 0 && disable(reinterpret_cast<void *>(plain_base + 1)) == 0 &&
                  graph.thread_notifications_enabled(plain_base) == true,
              "null/interior module handles fail without changing notification policy");
        check(disable(reinterpret_cast<void *>(base("tls.dll"))) == 0 && services::get_last_error() == 126 &&
                  graph.thread_notifications_enabled(base("tls.dll")) == true,
              "static-TLS module cannot disable thread callouts");
        check(disable(reinterpret_cast<void *>(base("app.exe"))) == 0,
              "executable handle cannot substitute for a DLL handle");
        {
            services::Scope nested(runtime);
            check(!services::current_modules(), "nested service scope does not inherit a stale graph");
        }
        check(services::current_modules() == &graph, "nested scope restores the original module graph");
        loader_reasons.clear();
        auto execution = graph.start();
        check(loader_reasons == std::vector<std::uint32_t>{1} && loader_disable_result == 1 &&
                  loader_disable_error == 49 && graph.thread_notifications_enabled(plain_base) == false,
              "DLL process attach disables its own thread policy through the Microsoft ABI");
        check(disable(reinterpret_cast<void *>(plain_base)) == 1 &&
                  graph.thread_notifications_enabled(plain_base) == false,
              "repeat notification disable is idempotent");
        execution.close();
        check(loader_reasons == std::vector<std::uint32_t>{1, 0},
              "disabled thread policy preserves DLL process detach");
    }
    check(!services::current_modules(), "module binding ends before graph destruction");
}
