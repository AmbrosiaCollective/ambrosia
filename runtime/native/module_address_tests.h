void module_address_checks() {
    using namespace ambrosia;
    pe::ModuleGraph graph(resolve);
    graph.add("app.exe", fixture());
    graph.add("provider.dll", provider_fixture());
    check(graph.module_base_for_address(1) == 0, "registered but unmapped modules have no address range");
    graph.load("app.exe");
    (void)graph.export_address("provider.dll", named_symbol("named_value"));
    const auto ranges = graph.diagnostic_ranges();
    bool boundaries = ranges.size() == 2;
    for (const auto &module : ranges) {
        boundaries = boundaries && graph.module_base_for_address(module.base) == module.base &&
                     graph.module_base_for_address(module.base + module.bytes - 1) == module.base &&
                     graph.module_base_for_address(module.base + 0x1800) == module.base;
    }
    check(boundaries, "module address lookup includes image starts, final bytes and internal section gaps");
    for (const auto &module : ranges) {
        const auto after = module.base + module.bytes;
        const auto expected = std::find_if(ranges.begin(), ranges.end(), [&](const auto &other) {
            return after >= other.base && after - other.base < other.bytes;
        });
        check(graph.module_base_for_address(after) == (expected == ranges.end() ? 0 : expected->base),
              "module address lookup excludes the end boundary without hiding adjacent images");
    }
    check(graph.module_base_for_address(0) == 0 && graph.module_base_for_address(UINTPTR_MAX) == 0,
          "unknown address edges return no module without pointer arithmetic overflow");
    services::Runtime runtime;
    services::Scope scope(runtime, &graph);
    using Lookup = void *(AMBROSIA_GUEST_ABI *)(void *, void *);
    const auto lookup = reinterpret_cast<Lookup>(
        runtime.resolve(service_symbol("RtlPcToFileHeader", "api-ms-win-core-rtlsupport-l1-1-0.dll")));

    struct Guarded {
        std::uint64_t before = 0x1234;
        void *base = nullptr;
        std::uint64_t after = 0x5678;
    } output;

    services::set_last_error(47);
    services::crt_error() = 31;
    const auto &first = ranges.front();
    const auto expected = reinterpret_cast<void *>(first.base);
    check(lookup(reinterpret_cast<void *>(first.base + 0x1000), &output.base) == expected &&
              output.base == expected && output.before == 0x1234 && output.after == 0x5678,
          "RtlPcToFileHeader Microsoft ABI returns/writes exactly the containing image base");
    check(!lookup(nullptr, &output.base) && !output.base,
          "unknown PC clears the output image base and returns null");
    check(services::get_last_error() == 47 && services::crt_error() == 31,
          "PC-to-image queries preserve Win32 and CRT errors");
    {
        auto execution = graph.start();
        check(graph.module_base_for_address(first.base + 0x1000) == first.base &&
                  lookup(reinterpret_cast<void *>(first.base), &output.base) == expected,
              "module address lookup remains available during an active execution session");
        execution.close();
    }
    check(graph.module_base_for_address(first.base) == first.base,
          "closing an execution session retains the graph's image mapping");
}
