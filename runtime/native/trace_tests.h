void trace_configuration_checks() {
    using ambrosia::diagnostics::ImportStops;
    using ambrosia::diagnostics::ImportStopScope;
    using ambrosia::pe::LoadMode;
    using ambrosia::pe::ModuleGraph;
    using ambrosia::pe::ModuleLimits;
    ImportStops stops;
    const auto symbol = service_symbol("missing");
    const auto first = stops.resolve(symbol);
    check(first && stops.resolve(symbol) == first && stops.size() == 1,
          "diagnostic import stops reuse one stable address per symbol");
    auto ordinal = symbol;
    ordinal.by_ordinal = true;
    ordinal.ordinal = 7;
    check(stops.resolve(ordinal) != first && stops.size() == 2,
          "named and ordinal diagnostic stops remain distinct");
    check(stops.label(0) == "TRACE unsupported import kernel32.dll!missing" && stops.label(2).empty(),
          "diagnostic labels are bounded and descriptive");
    check(runtime_rejected([&] { ImportStopScope unsealed(stops); }),
          "unsealed writable stop code cannot be activated");
    auto control = symbol;
    control.name = "bad\nname";
    check(runtime_rejected([&] { (void)stops.resolve(control); }),
          "diagnostic import labels cannot inject terminal controls");
    stops.seal();
    check(runtime_rejected([&] { (void)stops.resolve(symbol); }),
          "sealed executable stops cannot be modified");
    check(runtime_rejected([&] { stops.seal(); }), "stop sealing is an explicit one-way operation");
#if defined(__linux__)
    const auto address = reinterpret_cast<const std::byte *>(first);
    check(inspection_page_permissions(address).starts_with("r-x"),
          "sealed stop code is executable and not writable");
#endif
    { ImportStopScope scope(stops); }
    bool rejected_configuration = false;
    try {
        ModuleGraph invalid({}, {}, {}, [](const auto &) { return std::uintptr_t{1}; });
    } catch (const std::invalid_argument &) {
        rejected_configuration = true;
    }
    check(rejected_configuration, "normal executable graphs cannot install diagnostic import fallbacks");
    ModuleLimits limits;
    limits.image.mode = LoadMode::diagnostic_startup;
    std::size_t stop_calls = 0;
    ModuleGraph graph(
        {},
        [&](const auto &name) -> std::optional<Bytes> {
            return name == "provider.dll" ? std::optional(provider_fixture()) : std::nullopt;
        },
        limits,
        [&](const auto &) {
            ++stop_calls;
            return std::uintptr_t{1};
        });
    auto caller = fixture();
    text(caller, 0x440, "provider.dll");
    text(caller, 0x4a2, "named_value");
    graph.add("app.exe", caller);
    graph.load("app.exe");
    check(stop_calls == 0 && graph.statistics().modules == 2,
          "bundled DLL exports resolve before diagnostic missing-import fallback");
    const auto ranges = graph.diagnostic_ranges();
    check(ranges.size() == 2 && ranges[0].base && ranges[0].bytes,
          "diagnostic ranges retain loaded module names and extents");
    ModuleGraph missing({}, {}, limits, [&](const auto &) {
        ++stop_calls;
        return std::uintptr_t{1};
    });
    missing.add("app.exe", fixture());
    missing.load("app.exe");
    check(stop_calls == 1, "diagnostic fallback is requested only for an unresolved symbol");
}
