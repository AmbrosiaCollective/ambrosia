void module_handle_checks() {
    using namespace ambrosia;
    pe::ModuleGraph graph(resolve);
    graph.add("app.exe", fixture());
    graph.add("provider.dll", provider_fixture());
    graph.add("unmapped.dll", provider_fixture());
    check(!graph.loaded_module() && !graph.loaded_module("provider"),
          "module handle lookup does not map registered source images");
    graph.load("app.exe");
    (void)graph.export_address("provider.dll", named_symbol("named_value"));
    const auto root = graph.loaded_module();
    const auto provider = graph.loaded_module("provider.dll");
    check(root && provider && root != provider && graph.loaded_module("APP.EXE") == root &&
              graph.loaded_module("PROVIDER") == provider,
          "module handle lookup uses the actual root and case-insensitive default-extension DLL names");
    check(!graph.loaded_module("unmapped.dll") && !graph.loaded_module("absent.dll") &&
              graph.statistics().modules == 2,
          "module handle queries neither load source-only images nor create missing dependencies");
    services::Runtime runtime;
    check(runtime.has_service_module("KERNEL32") && !runtime.has_service_module("absent.dll"),
          "native service catalog is distinct from mapped PE modules");
    services::Scope scope(runtime, &graph);
    using Narrow = void *(AMBROSIA_GUEST_ABI *)(const char *);
    using Wide = void *(AMBROSIA_GUEST_ABI *)(const char16_t *);
    const auto narrow = reinterpret_cast<Narrow>(
        runtime.resolve(service_symbol("GetModuleHandleA", "api-ms-win-core-libraryloader-l1-2-0.dll")));
    const auto wide =
        reinterpret_cast<Wide>(runtime.resolve(service_symbol("GetModuleHandleW", "kernel32.dll")));
    services::set_last_error(64);
    services::crt_error() = 17;
    check(narrow(nullptr) == reinterpret_cast<void *>(root) &&
              wide(nullptr) == reinterpret_cast<void *>(root),
          "GetModuleHandle Microsoft ABI null names return the root executable base");
    check(narrow("PROVIDER") == reinterpret_cast<void *>(provider) &&
              wide(u"PrOvIdEr.DlL") == reinterpret_cast<void *>(provider),
          "ANSI UTF-8 and UTF-16 module names select the same loaded image");
    check(services::get_last_error() == 64 && services::crt_error() == 17,
          "successful module handles preserve Win32 and CRT errors");
    check(!wide(u"absent.dll") && services::get_last_error() == 126 && services::crt_error() == 17,
          "unknown module returns null and module-not-found without loading a file");
    check(!narrow("") && services::get_last_error() == 126,
          "empty module name does not substitute for a null root query");
    auto execution = graph.start();
    check(wide(u"PROVIDER") == reinterpret_cast<void *>(provider) && graph.loaded_module() == root,
          "module handles remain stable while the graph is executing");
    execution.close();
}
