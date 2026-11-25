void processor_topology_checks() {
    using namespace ambrosia;
    using namespace services;
    const auto root = std::filesystem::temp_directory_path() / "ambrosia-topology-fixture";
    check(!std::filesystem::exists(root), "topology fixture owns a fresh temporary directory");
    const auto write = [&](const std::filesystem::path &name, const std::string &value) {
        const auto path = root / name;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary);
        stream << value << '\n';
        if (!stream) {
            throw std::runtime_error("topology fixture write failed");
        }
    };
    write("cpu/online", "0,2,9");
    write("node/online", "0,2");
    write("node/node0/cpulist", "0,2");
    write("node/node2/cpulist", "9");
    for (unsigned id : {0u, 2u, 9u}) {
        const auto cpu = std::filesystem::path("cpu") / ("cpu" + std::to_string(id));
        write(cpu / "topology/core_id", "0");
        write(cpu / "topology/physical_package_id", id == 9 ? "1" : "0");
        const auto cache = cpu / "cache/index0";
        write(cache / "type", "Data");
        write(cache / "level", "1");
        write(cache / "ways_of_associativity", "8");
        write(cache / "number_of_sets", "64");
        write(cache / "coherency_line_size", "64");
        write(cache / "size", "32K");
        write(cache / "shared_cpu_list", id == 9 ? "9" : "0,2");
    }
    const auto topology = host::read_linux_topology(root, {9, 2, 0});
    check(topology.processor_ids == std::vector<unsigned>{0, 2, 9} && topology.cores.size() == 2 &&
              topology.cores[0].mask == 3 && topology.cores[1].mask == 4,
          "sparse host IDs become dense guest bits and core IDs remain package-qualified");
    check(topology.packages.size() == 2 && topology.packages[0].mask == 3 && topology.packages[1].mask == 4 &&
              topology.nodes.size() == 2 && topology.nodes[1].id == 2 && topology.nodes[1].mask == 4,
          "physical packages and actual noncontiguous NUMA node IDs retain distinct relationships");
    check(topology.caches.size() == 2 && topology.caches[0].mask == 3 && topology.caches[0].bytes == 32768 &&
              topology.caches[0].line_bytes == 64 && topology.caches[0].ways == 8 &&
              topology.caches[1].mask == 4,
          "shared cache reports deduplicate with exact geometry and guest processor masks");
    check(runtime_rejected([&] { host::read_linux_topology(root, {}); }) &&
              runtime_rejected([&] { host::read_linux_topology(root, {0, 0}); }) &&
              runtime_rejected([&] { host::read_linux_topology(root, {1}); }) &&
              runtime_rejected([&] { host::read_linux_topology(root, std::vector<unsigned>(65)); }),
          "empty, duplicate, offline and multi-group selections are refused without truncation");
    for (const auto *invalid : {"0,", "0,,2", "0-2,2", "2-0", "-1", "0,4096", "0x2"}) {
        write("cpu/online", invalid);
        check(runtime_rejected([&] { host::read_linux_topology(root, {0}); }),
              "malformed or oversized processor lists fail closed");
    }
    write("cpu/online", "0-2,9");
    check(host::read_linux_topology(root, {0, 2, 9}).processor_ids == topology.processor_ids,
          "CPU list ranges do not invent selected processors or preserve offline gaps");
    write("node/node2/cpulist", "0,9");
    check(runtime_rejected([&] { host::read_linux_topology(root, {0, 2, 9}); }),
          "overlapping NUMA ownership is refused");
    write("node/node2/cpulist", "\n");
    check(runtime_rejected([&] { host::read_linux_topology(root, {0, 2, 9}); }),
          "missing NUMA coverage is refused instead of assuming node zero");
    write("node/node2/cpulist", "9");
    write("cpu/cpu2/cache/index0/size", "64K");
    check(runtime_rejected([&] { host::read_linux_topology(root, {0, 2, 9}); }),
          "conflicting reports for one shared cache fail closed");
    write("cpu/cpu2/cache/index0/size", "32K");
    write("cpu/cpu0/cache/index0/shared_cpu_list", "9");
    check(runtime_rejected([&] { host::read_linux_topology(root, {0, 2, 9}); }),
          "a cache must include its reporting processor");
    write("cpu/cpu0/cache/index0/shared_cpu_list", "0,2");
    write("cpu/cpu0/topology/core_id", std::string(4097, '1'));
    check(runtime_rejected([&] { host::read_linux_topology(root, {0}); }),
          "topology text reads have a fixed size bound");
    write("cpu/cpu0/topology/core_id", "0");
    write("cpu/cpu9/cache/index0/number_of_sets", "1");
    check(host::read_linux_topology(root, {9}).caches.front().fully_associative,
          "single-set caches are identified as fully associative from actual metadata");
    std::filesystem::remove_all(root / "cpu/cpu0/cache");
    check(host::read_linux_topology(root, {0}).caches.empty(),
          "unreported caches are permitted without fabricated descriptors");
    std::filesystem::remove_all(root);

    ProcessorInformation information(topology);
    std::uint32_t bytes = 0;
    check(information.copy(nullptr, bytes) == 122 && bytes == 256,
          "processor information query returns the exact Windows x64 array size");
    std::vector<std::byte> output(bytes + 2, std::byte{0x5a});
    auto capacity = bytes - 1;
    const auto original = output;
    check(information.copy(output.data() + 1, capacity) == 122 && capacity == bytes && output == original,
          "short buffers report required bytes and remain entirely untouched");
    capacity = bytes + 1;
    check(information.copy(output.data() + 1, capacity) == 0 && capacity == bytes &&
              output.front() == std::byte{0x5a} && output.back() == std::byte{0x5a},
          "unaligned output copies exactly the required bytes and preserves guards");
    std::array<LogicalProcessorInformation, 8> records{};
    std::memcpy(records.data(), output.data() + 1, bytes);
    check(records[0].mask == 3 && records[0].relationship == 0 && records[0].data[0] == std::byte{1} &&
              records[1].mask == 4 && records[1].data[0] == std::byte{0} && records[2].relationship == 1 &&
              records[3].data[0] == std::byte{2} && records[4].relationship == 3,
          "x64 core SMT flags, NUMA IDs and package relationships occupy the documented offsets");
    const auto *cache = reinterpret_cast<const std::byte *>(&records[6]);
    check(records[6].relationship == 2 && cache[16] == std::byte{1} && cache[17] == std::byte{8} &&
              cache[18] == std::byte{64} && cache[21] == std::byte{0x80} && cache[24] == std::byte{2} &&
              records[6].padding == 0 && cache[28] == std::byte{0} && cache[31] == std::byte{0},
          "cache descriptor fields and reserved bytes use Windows x64 layout without host padding leaks");
    auto invalid = topology;
    invalid.cores[1].mask = 3;
    check(runtime_rejected([&] { ProcessorInformation rejected(invalid); }),
          "overlapping core partitions cannot reach guest ABI output");
    invalid = topology;
    invalid.nodes[1].id = 0;
    check(runtime_rejected([&] { ProcessorInformation rejected(invalid); }),
          "duplicate NUMA node IDs cannot reach guest ABI output");
    invalid = topology;
    invalid.caches[0].line_bytes = 65536;
    check(runtime_rejected([&] { ProcessorInformation rejected(invalid); }),
          "cache geometry is rejected rather than narrowed to the guest field width");

    invalid = topology;
    invalid.caches[0].ways = 255;
    check(runtime_rejected([&] { ProcessorInformation rejected(invalid); }),
          "a raw way count cannot collide with the fully-associative ABI sentinel");
    invalid.caches[0].fully_associative = true;
    ProcessorInformation associative(invalid);
    capacity = bytes;
    associative.copy(records.data(), capacity);
    check(records[6].data[1] == std::byte{255},
          "fully-associative caches use the ABI sentinel rather than a narrowed way count");
    host::ProcessorTopology largest;
    for (unsigned id = 0; id < 64; ++id) {
        largest.processor_ids.push_back(id);
    }
    largest.cores.push_back({0, UINT64_MAX});
    largest.packages.push_back({0, UINT64_MAX});
    largest.nodes.push_back({0, UINT64_MAX});
    ProcessorInformation full_group(largest);
    capacity = sizeof(records);
    check(!full_group.copy(records.data(), capacity) && capacity == 96 && records[0].mask == UINT64_MAX,
          "the largest supported group preserves bit 63 without an overflowing shift");

    Runtime runtime(MemoryLimits{1 << 20, 1 << 20, 1 << 20}, topology);
    Scope scope(runtime);
    using Query = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, std::uint32_t *);
    const auto symbol =
        service_symbol("GetLogicalProcessorInformation", "api-ms-win-core-sysinfo-l1-1-0.dll");
    const auto address = runtime.resolve(symbol);
    const auto query = reinterpret_cast<Query>(address);
    check(address &&
              runtime.resolve(service_symbol("GetLogicalProcessorInformation", "kernel32.dll")) == address &&
              runtime.resolve(service_symbol("GetLogicalProcessorInformation", "kernelbase.dll")) == address,
          "processor information resolves to one callable native facade across its API aliases");
    set_last_error(73);
    crt_error() = 41;
    check(query(nullptr, nullptr) == 0 && get_last_error() == 87 && crt_error() == 41,
          "null length returns invalid parameter without changing CRT errno");
    bytes = 0;
    check(query(nullptr, &bytes) == 0 && bytes == 256 && get_last_error() == 122,
          "processor-information Microsoft ABI supports the required-length probe");
    set_last_error(73);
    check(query(records.data(), &bytes) == 1 && bytes == 256 && get_last_error() == 73 && crt_error() == 41,
          "successful topology query preserves LastError and CRT errno");
}
