void native_module_checks() {
    using namespace ambrosia;
    using namespace services;
    const auto target = reinterpret_cast<std::uintptr_t>(&sum6);
    ServiceModuleImage image("ambrosia-test.dll", {{"sum6", target, {}}});
    const auto bytes = image.bytes();
    const auto nt = pe::detail::read<std::uint32_t>(bytes, 0x3c);
    const auto optional = nt + 24;
    check(pe::detail::read<std::uint16_t>(bytes, 0) == 0x5a4d &&
              pe::detail::read<std::uint32_t>(bytes, nt) == 0x4550 &&
              pe::detail::read<std::uint16_t>(bytes, nt + 4) == 0x8664 &&
              pe::detail::read<std::uint16_t>(bytes, optional) == 0x20b,
          "native service image has real AMD64 PE32+ DOS and NT headers");
    check(pe::detail::read<std::uint64_t>(bytes, optional + 24) == image.base() &&
              pe::detail::read<std::uint32_t>(bytes, optional + 56) == bytes.size() &&
              pe::detail::read<std::uint32_t>(bytes, optional + 16) == 0,
          "native service metadata reports its actual mapped base/extent and no fake entrypoint");
    const auto export_rva = pe::detail::read<std::uint32_t>(bytes, optional + 112);
    const auto export_size = pe::detail::read<std::uint32_t>(bytes, optional + 116);
    const auto parsed = pe::parse_exports(bytes, export_rva, export_size);
    const auto entry = image.direct_export("sum6");
    check(parsed.names.size() == 1 && parsed.names.at("sum6").rva == entry - image.base() &&
              parsed.names.at("sum6").forwarder.empty(),
          "checked PE export parser independently resolves the emitted native trampoline RVA");
    using Sum = std::uint64_t(AMBROSIA_GUEST_ABI *)(std::uint64_t, std::uint64_t, std::uint64_t,
                                                    std::uint64_t, std::uint64_t, std::uint64_t);
    saw_arguments = false;
    check(reinterpret_cast<Sum>(entry)(1, 2, 3, 4, 5, 6) == 21 && saw_arguments,
          "native service tail trampoline preserves register/stack arguments and Microsoft ABI return");
#if defined(__linux__)
    check(inspection_page_permissions(bytes.data()).starts_with("r--") &&
              inspection_page_permissions(reinterpret_cast<const std::byte *>(entry)).starts_with("r-x") &&
              inspection_page_permissions(bytes.data() + export_rva).starts_with("r--"),
          "native module headers/exports are read-only and callable code is RX without writable execution");
#endif
    ServiceModuleImage forward("forward.dll", {{"forward_sum", 0, "ambrosia-test.sum6"}});
    check(forward.exports().names.at("forward_sum").forwarder == "ambrosia-test.sum6" &&
              !forward.direct_export("forward_sum"),
          "service facade emits a real PE forwarder instead of an opaque function token");
    check(
        runtime_rejected(
            [&] { ServiceModuleImage duplicate("bad.dll", {{"f", target, {}}, {"f", target, {}}}); }) &&
            runtime_rejected([&] { ServiceModuleImage ambiguous("bad.dll", {{"f", target, "other.f"}}); }) &&
            runtime_rejected([&] { ServiceModuleImage empty_target("bad.dll", {{"f", 0, {}}}); }),
        "native module builder rejects duplicated, ambiguous and missing export targets");
    check(runtime_rejected([&] { ServiceModuleImage invalid("../bad.dll", {{"f", target, {}}}); }) &&
              runtime_rejected(
                  [&] { ServiceModuleImage long_name("bad.dll", {{std::string(513, 'f'), target, {}}}); }) &&
              runtime_rejected(
                  [&] { ServiceModuleImage too_many("bad.dll", std::vector<ServiceExport>(4097)); }),
          "native module metadata enforces basename, string and export-count budgets");

    ImportRegistry registry;
    registry.add("first.dll", "sum6", target);
    registry.add("second.dll", "same_target", target);
    NativeModules modules(registry);
    const auto first = modules.handle("FIRST");
    const auto second = modules.handle("second.dll");
    const auto canonical = modules.handle("ambrosia-native.dll");
    const auto canonical_entry = modules.entrypoint(target);
    check(first && second && canonical && first != second && first != canonical &&
              modules.handle("first.dll") == first && !modules.handle("absent.dll"),
          "native service facade handles are real, stable and distinct with no invented missing module");
    check(modules.export_address(first, "sum6") == canonical_entry &&
              modules.export_address(second, "same_target") == canonical_entry &&
              modules.module_base_for_address(canonical_entry) == canonical,
          "module aliases share one canonical callable pointer with a real containing PE image");
    check(modules.export_address(first, "absent") == 0 && !modules.export_address(1, "sum6").has_value() &&
              runtime_rejected([&] { (void)modules.entrypoint(1); }),
          "native module lookup distinguishes absent exports, invalid handles and unregistered targets");

    Runtime runtime;
    pe::ModuleGraph graph(resolve);
    graph.add("app.exe", fixture());
    graph.load("app.exe");
    Scope scope(runtime, &graph);
    using GetModule = void *(AMBROSIA_GUEST_ABI *)(const char16_t *);
    using PcLookup = void *(AMBROSIA_GUEST_ABI *)(void *, void *);
    const auto get_module =
        reinterpret_cast<GetModule>(runtime.resolve(service_symbol("GetModuleHandleW", "kernel32.dll")));
    const auto get_error = runtime.resolve(service_symbol("GetLastError", "kernel32.dll"));
    const auto lookup_pc =
        reinterpret_cast<PcLookup>(runtime.resolve(service_symbol("RtlPcToFileHeader", "ntdll.dll")));
    auto *kernel = get_module(u"KERNEL32");
    check(kernel &&
              runtime.native_modules().export_address(reinterpret_cast<std::uintptr_t>(kernel),
                                                      "GetLastError") == get_error &&
              runtime.resolve(service_symbol("GetLastError", "kernelbase.dll")) == get_error,
          "GetModuleHandle native facade exports preserve static-import and alias pointer identity");
    void *base = nullptr;
    check(lookup_pc(reinterpret_cast<void *>(get_error), &base) == base && base &&
              reinterpret_cast<std::uintptr_t>(base) ==
                  runtime.native_modules().handle("ambrosia-native.dll"),
          "RtlPcToFileHeader finds the canonical image owning a native adapter trampoline");

    using GetProc = void *(AMBROSIA_GUEST_ABI *)(void *, const char *);
    const auto get_proc =
        reinterpret_cast<GetProc>(runtime.resolve(service_symbol("GetProcAddress", "kernel32.dll")));
    set_last_error(65);
    crt_error() = 29;
    check(reinterpret_cast<std::uintptr_t>(get_proc(kernel, "GetLastError")) == get_error &&
              get_last_error() == 65 && crt_error() == 29,
          "GetProcAddress native named exports match the static canonical pointer and preserve errors");
    check(!get_proc(kernel, "NotImplementedByThisRuntime") && get_last_error() == 127 &&
              !get_proc(reinterpret_cast<void *>(1), "GetLastError") && get_last_error() == 6,
          "dynamic lookup distinguishes a genuinely absent facade export from an invalid module handle");

    pe::ModuleGraph exports_graph;
    auto forwarded = exported_fixture(provider_fixture(), {{"forwarded", "late.named_value", 0}});
    exports_graph.add("forward.dll", forwarded);
    exports_graph.add("late.dll", provider_fixture());
    exports_graph.load("forward.dll");
    const auto forward_handle = exports_graph.loaded_module();
    check(exports_graph.loaded_export(forward_handle, named_symbol("forwarded")) == 0 &&
              exports_graph.statistics().modules == 1 && !exports_graph.loaded_module("late.dll"),
          "read-only dynamic forwarder lookup never maps a source-only dependency");
    const auto late_function = exports_graph.export_address("late.dll", named_symbol("named_value"));
    const auto late_handle = exports_graph.loaded_module("late.dll");
    auto ordinal = named_symbol({});
    ordinal.by_ordinal = true;
    ordinal.ordinal = 77;
    check(exports_graph.loaded_export(late_handle, ordinal) == late_function &&
              exports_graph.loaded_export(forward_handle, named_symbol("forwarded")) == late_function &&
              !exports_graph.loaded_export(1, ordinal).has_value(),
          "loaded PE exports support real ordinals and forwarders without accepting unknown handles");
    {
        Scope loaded_scope(runtime, &exports_graph);
        auto execution = exports_graph.start();
        const auto resolved =
            get_proc(reinterpret_cast<void *>(late_handle), reinterpret_cast<const char *>(77));
        check(reinterpret_cast<std::uintptr_t>(resolved) == late_function &&
                  get_proc(reinterpret_cast<void *>(forward_handle), "forwarded") == resolved,
              "GetProcAddress resolves real PE ordinals/forwarders during active module execution");
        check(reinterpret_cast<std::uint64_t(AMBROSIA_GUEST_ABI *)()>(resolved)() == 42,
              "dynamic PE procedure pointer calls the original synthetic export");
    }
}
