#include "forwarder_cases.h"

struct FixtureExport {
    std::string name;
    std::string forwarder;
    std::uint32_t rva = 0x1000;
};

Bytes exported_fixture(Bytes bytes, const std::vector<FixtureExport> &exports,
                       std::uint32_t ordinal_base = 1) {
    constexpr std::size_t directory = 0x640;
    const auto functions = directory + 40;
    const auto names = functions + exports.size() * 4;
    const auto ordinals = names + exports.size() * 4;
    auto cursor = ordinals + exports.size() * 2;
    auto rva = [](std::size_t offset) { return static_cast<std::uint32_t>(0x3000 + offset - 0x600); };
    auto append = [&](const std::string &value) {
        if (value.size() + 1 > bytes.size() - cursor) {
            throw std::runtime_error("synthetic export table exceeds fixture section");
        }
        const auto address = rva(cursor);
        text(bytes, cursor, value.c_str());
        cursor += value.size() + 1;
        return address;
    };
    put<std::uint32_t>(bytes, 0x98 + 112, rva(directory));
    put<std::uint32_t>(bytes, directory + 16, ordinal_base);
    put<std::uint32_t>(bytes, directory + 20, exports.size());
    put<std::uint32_t>(bytes, directory + 24, exports.size());
    put<std::uint32_t>(bytes, directory + 28, rva(functions));
    put<std::uint32_t>(bytes, directory + 32, rva(names));
    put<std::uint32_t>(bytes, directory + 36, rva(ordinals));
    std::vector<std::size_t> sorted;
    for (std::size_t index = 0; index < exports.size(); ++index) {
        sorted.push_back(index);
        const auto target =
            exports[index].forwarder.empty() ? exports[index].rva : append(exports[index].forwarder);
        put<std::uint32_t>(bytes, functions + index * 4, target);
    }
    std::sort(sorted.begin(), sorted.end(),
              [&](auto left, auto right) { return exports[left].name < exports[right].name; });
    for (std::size_t index = 0; index < sorted.size(); ++index) {
        put<std::uint32_t>(bytes, names + index * 4, append(exports[sorted[index]].name));
        put<std::uint16_t>(bytes, ordinals + index * 2, sorted[index]);
    }
    put<std::uint32_t>(bytes, 0x98 + 116, cursor - directory);
    return bytes;
}

Bytes provider_fixture() {
    auto bytes = fixture();
    put<std::uint16_t>(bytes, 0x96, 0x2022);
    put<std::uint32_t>(bytes, 0x98 + 16, 0);
    put<std::uint32_t>(bytes, 0x98 + 120, 0);
    put<std::uint32_t>(bytes, 0x98 + 124, 0);
    const unsigned char entry[] = {0xb8, 42, 0, 0, 0, 0xc3};
    std::memcpy(bytes.data() + 0x200, entry, sizeof(entry));
    return exported_fixture(std::move(bytes), {{"named_value", {}, 0x1000}}, 77);
}

ambrosia::pe::Import named_symbol(std::string name) {
    ambrosia::pe::Import symbol;
    symbol.name = std::move(name);
    return symbol;
}

void export_checks() {
    auto bytes = provider_fixture();
    auto provider = ambrosia::pe::Image::load(bytes, {});
    auto named = named_symbol("named_value");
    auto ordinal = named_symbol({});
    ordinal.by_ordinal = true;
    ordinal.ordinal = 77;
    check(provider.export_address(named) == reinterpret_cast<std::uintptr_t>(provider.base() + 0x1000),
          "named export points into mapped image");
    check(provider.export_address(ordinal) == provider.export_address(named),
          "nonzero ordinal base resolves same export");
    check(provider.export_address(named_symbol("absent")) == 0, "missing export is explicit");
    auto malformed = [&](auto mutation, const char *label) {
        auto bad = bytes;
        mutation(bad);
        check(rejected(bad), label);
    };
    malformed([](auto &b) { put<std::uint32_t>(b, 0x98 + 116, 39); }, "short export directory rejected");
    malformed([](auto &b) { put<std::uint32_t>(b, 0x640 + 20, 65537); }, "export count bounded");
    malformed([](auto &b) { put<std::uint16_t>(b, 0x670, 1); }, "name ordinal outside export table rejected");
    malformed([](auto &b) { put<std::uint32_t>(b, 0x668, 0x4000); }, "export outside mapping rejected");
    malformed([](auto &b) { put<std::uint32_t>(b, 0x668, 0x2800); },
              "export in unmapped section gap rejected");
    auto unused = bytes;
    put<std::uint32_t>(unused, 0x668, 0);
    check(ambrosia::pe::Image::load(unused, {}).export_address(named) == 0,
          "unused export address slot remains absent");
    auto forward = exported_fixture(provider_fixture(), {{"forwarded", "provider.named_value", 0}});
    put<std::uint32_t>(forward, 0x98 + 116, 51);
    check(rejected(forward), "forwarder string cannot escape export directory bounds");
}

void forwarder_checks() {
    using ambrosia::pe::ModuleGraph;
    auto forward = provider_fixture();
    std::vector<FixtureExport> exports;
    for (const auto &item : forwarder_cases) {
        exports.push_back({item.name, item.target, 0});
    }
    forward = exported_fixture(std::move(forward), exports);
    for (const auto &item : forwarder_cases) {
        auto caller = fixture();
        text(caller, 0x440, "intake_forward.dll");
        text(caller, 0x4a2, item.name);
        ModuleGraph graph;
        graph.add("app.exe", caller);
        graph.add("intake_forward.dll", forward);
        graph.add("intake_provider.dll", provider_fixture());
        bool resolved = false;
        try {
            graph.load("APP.EXE");
            auto execution = graph.start();
            resolved = execution.run_bootstrap() == 42;
        } catch (const std::runtime_error &) {
            resolved = false;
        }
        check(resolved == item.resolves, item.name);
        if (resolved) {
            check(graph.initialization_order() ==
                      std::vector<std::string>{"intake_provider.dll", "intake_forward.dll", "app.exe"},
                  "forwarder dependency initialized before importer");
        }
    }
    ModuleGraph duplicate;
    duplicate.add("provider", provider_fixture());
    bool rejected_duplicate = false;
    try {
        duplicate.add("PROVIDER.DLL", provider_fixture());
    } catch (const std::runtime_error &) {
        rejected_duplicate = true;
    }
    check(rejected_duplicate, "case-insensitive duplicate module rejected");
    ModuleGraph traversal;
    bool rejected_path = false;
    try {
        traversal.add("../provider.dll", provider_fixture());
    } catch (const std::runtime_error &) {
        rejected_path = true;
    }
    check(rejected_path, "module registration rejects host path traversal");
    auto cyclic = fixture();
    text(cyclic, 0x440, "app.exe");
    ModuleGraph cycle;
    cycle.add("app.exe", cyclic);
    bool rejected_cycle = false;
    try {
        cycle.load("app.exe");
    } catch (const std::runtime_error &) {
        rejected_cycle = true;
    }
    bool rejected_failed_start = false;
    try {
        auto execution = cycle.start();
    } catch (const std::runtime_error &) {
        rejected_failed_start = true;
    }
    check(rejected_cycle && rejected_failed_start, "cyclic imports leave graph failed closed");
}

std::vector<std::pair<std::uint32_t, std::uint32_t>> graph_events;
bool fail_graph_attach = false;

std::uint32_t *graph_tls(void *module) {
    std::uint32_t slot = 0;
    std::memcpy(&slot, static_cast<std::byte *>(module) + 0x2170, sizeof(slot));
#if defined(__linux__) && defined(__x86_64__)
    void **slots = nullptr;
    __asm__ volatile("movq %%gs:0x58, %0" : "=r"(slots));
    return static_cast<std::uint32_t *>(slots[slot]);
#else
    return nullptr;
#endif
}

void AMBROSIA_GUEST_ABI graph_callback(void *module, std::uint32_t reason, void *) {
    graph_events.emplace_back(graph_tls(module)[0], reason);
}

std::int32_t AMBROSIA_GUEST_ABI graph_dll_entry(void *, std::uint32_t reason, void *) {
    return !(fail_graph_attach && reason == 1);
}

void graph_tls_checks() {
#if defined(__linux__) && defined(__x86_64__)
    using ambrosia::pe::ModuleGraph;
    auto dependency = tls_fixture(true);
    put<std::uint32_t>(dependency, 0x98 + 16, 0);
    put<std::uint32_t>(dependency, 0x98 + 120, 0);
    put<std::uint32_t>(dependency, 0x98 + 124, 0);
    put<std::uint32_t>(dependency, 0x560, 100);
    jump_to_host(dependency, 0x300, reinterpret_cast<std::uintptr_t>(&graph_callback));
    // Read this module's assigned index, then its block from the shared vector.
    const unsigned char entry[] = {0x8b, 0x0d, 0,    0,    0,    0,    0x65, 0x48, 0x8b, 0x04, 0x25, 0x58, 0,
                                   0,    0,    0x48, 0x8b, 0x04, 0xc8, 0x83, 0,    1,    0x8b, 0,    0xc3};
    std::memcpy(dependency.data() + 0x200, entry, sizeof(entry));
    put<std::uint32_t>(dependency, 0x202, 0x2170 - 0x1006);
    dependency = exported_fixture(std::move(dependency), {{"named_value", {}, 0x1000}}, 77);
    auto root = tls_fixture();
    text(root, 0x440, "provider.dll");
    text(root, 0x4a2, "named_value");
    jump_to_host(root, 0x300, reinterpret_cast<std::uintptr_t>(&graph_callback));
    const unsigned char caller[] = {0x48, 0x83, 0xec, 0x28, 0xff, 0x15, 0,   0,
                                    0,    0,    0x48, 0x83, 0xc4, 0x28, 0xc3};
    std::memcpy(root.data() + 0x200, caller, sizeof(caller));
    put<std::uint32_t>(root, 0x206, 0x2080 - 0x100a);
    ModuleGraph graph;
    graph.add("app.exe", root);
    graph.add("provider.dll", dependency);
    graph.load("app.exe");
    graph_events.clear();
    const auto saved_gs = ambrosia::host::guest_segment_base();
    {
        auto execution = graph.start();
        check(graph_events == std::vector<std::pair<std::uint32_t, std::uint32_t>>{{100, 1}, {41, 1}},
              "dependency TLS callback precedes root callback");
        check(execution.run_bootstrap() == 101 && execution.run_bootstrap() == 102,
              "EXE-to-DLL call uses independent shared TLS slots");
        bool duplicate = false;
        try {
            auto other = graph.start();
        } catch (const std::runtime_error &) {
            duplicate = true;
        }
        check(duplicate && execution.run_bootstrap() == 103,
              "duplicate graph session leaves current execution intact");
    }
    check(graph_events ==
              std::vector<std::pair<std::uint32_t, std::uint32_t>>{{100, 1}, {41, 1}, {41, 0}, {103, 0}},
          "module detach runs in reverse dependency order with TLS intact");
    {
        auto execution = graph.start();
        check(execution.run_bootstrap() == 101, "new graph session receives fresh per-module TLS");
    }
    check(ambrosia::host::guest_segment_base() == saved_gs, "graph execution restores host GS");

    auto failing_root = root;
    put<std::uint16_t>(failing_root, 0x96, 0x2022);
    jump_to_host(failing_root, 0x200, reinterpret_cast<std::uintptr_t>(&graph_dll_entry));
    ModuleGraph failing;
    failing.add("app.dll", failing_root);
    failing.add("provider.dll", dependency);
    failing.load("app.dll");
    graph_events.clear();
    fail_graph_attach = true;
    bool rejected_attach = false;
    try {
        auto execution = failing.start();
    } catch (const std::runtime_error &) {
        rejected_attach = true;
    }
    fail_graph_attach = false;
    check(rejected_attach &&
              graph_events ==
                  std::vector<std::pair<std::uint32_t, std::uint32_t>>{{100, 1}, {41, 1}, {41, 0}, {100, 0}},
          "failed root attach rolls back initialized dependency");
    { auto retry = failing.start(); }
    check(ambrosia::host::guest_segment_base() == saved_gs,
          "failed graph startup releases lease and restores host GS");
#endif
}
