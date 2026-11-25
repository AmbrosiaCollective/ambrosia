#pragma once
#include <cstdint>
#include <filesystem>
#include <vector>

namespace ambrosia::host {
struct ProcessorGroup {
    std::uint32_t id;
    std::uint64_t mask;
};

enum class CacheKind { unified, instruction, data };

struct ProcessorCache {
    std::uint64_t mask;
    std::uint32_t level;
    std::uint32_t ways;
    std::uint32_t line_bytes;
    std::uint32_t bytes;
    CacheKind kind;
    bool fully_associative = false;
    bool operator==(const ProcessorCache &) const = default;
};

struct ProcessorTopology {
    // Guest bit N corresponds to host processor_ids[N], never a truncated host ID.
    std::vector<unsigned> processor_ids;
    std::vector<ProcessorGroup> cores;
    std::vector<ProcessorGroup> packages;
    std::vector<ProcessorGroup> nodes;
    std::vector<ProcessorCache> caches;
};

// Reads only CPU/NUMA metadata. A root parameter permits original tiny fixtures
// without changing host affinity or pretending a processor count is a topology.
ProcessorTopology read_linux_topology(const std::filesystem::path &system_root,
                                      const std::vector<unsigned> &allowed);
ProcessorTopology processor_topology();
} // namespace ambrosia::host
