#include "processor_topology.h"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#ifdef __linux__
#include <sched.h>
#endif

namespace ambrosia::host {
namespace {
unsigned number(std::string_view text) {
    unsigned value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        throw std::runtime_error("invalid CPU topology number");
    }
    return value;
}

std::string read_text(const std::filesystem::path &path) {
    std::ifstream stream(path);
    std::string text;
    char value;
    while (stream.get(value)) {
        if (text.size() == 4096) {
            throw std::runtime_error("CPU topology field exceeds its limit");
        }
        text.push_back(value);
    }
    if (!stream.eof() || text.empty()) {
        throw std::runtime_error("CPU topology metadata unavailable");
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

std::set<unsigned> cpu_list(std::string_view text) {
    std::set<unsigned> result;
    while (!text.empty()) {
        const auto comma = text.find(',');
        const auto item = text.substr(0, comma);
        const auto dash = item.find('-');
        const auto first = number(item.substr(0, dash));
        const auto last = dash == item.npos ? first : number(item.substr(dash + 1));
        if (first > last || last >= 4096) {
            throw std::runtime_error("CPU topology list exceeds supported IDs");
        }
        for (unsigned id = first; id <= last; ++id) {
            if (!result.insert(id).second) {
                throw std::runtime_error("CPU topology list contains duplicate IDs");
            }
        }
        if (comma == text.npos) {
            break;
        }
        text.remove_prefix(comma + 1);
        if (text.empty()) {
            throw std::runtime_error("CPU topology list has an empty item");
        }
    }
    return result;
}

std::uint64_t guest_mask(const std::set<unsigned> &ids, const std::vector<unsigned> &selected) {
    std::uint64_t mask = 0;
    for (std::size_t bit = 0; bit < selected.size(); ++bit) {
        if (ids.contains(selected[bit])) {
            mask |= std::uint64_t{1} << bit;
        }
    }
    return mask;
}

std::uint32_t cache_bytes(std::string text) {
    unsigned multiplier = 1;
    if (!text.empty() && text.back() == 'K') {
        multiplier = 1024;
        text.pop_back();
    } else if (!text.empty() && text.back() == 'M') {
        multiplier = 1024 * 1024;
        text.pop_back();
    }
    const auto value = number(text);
    if (!value || value > UINT32_MAX / multiplier) {
        throw std::runtime_error("CPU cache size exceeds the ABI range");
    }
    return value * multiplier;
}
} // namespace

ProcessorTopology read_linux_topology(const std::filesystem::path &root,
                                      const std::vector<unsigned> &allowed) {
    if (allowed.empty() || allowed.size() > 64) {
        throw std::runtime_error("native topology requires one group of 1 through 64 processors");
    }
    ProcessorTopology result;
    result.processor_ids = allowed;
    std::sort(result.processor_ids.begin(), result.processor_ids.end());
    if (std::adjacent_find(result.processor_ids.begin(), result.processor_ids.end()) !=
        result.processor_ids.end()) {
        throw std::runtime_error("duplicate allowed processor ID");
    }
    const auto online = cpu_list(read_text(root / "cpu/online"));
    std::map<std::pair<unsigned, unsigned>, std::uint64_t> cores;
    std::map<unsigned, std::uint64_t> packages;
    for (std::size_t bit = 0; bit < result.processor_ids.size(); ++bit) {
        const auto id = result.processor_ids[bit];
        if (!online.contains(id)) {
            throw std::runtime_error("allowed processor is not online");
        }
        const auto cpu = root / "cpu" / ("cpu" + std::to_string(id));
        const auto package = number(read_text(cpu / "topology/physical_package_id"));
        const auto core = number(read_text(cpu / "topology/core_id"));
        const auto mask = std::uint64_t{1} << bit;
        cores[{package, core}] |= mask;
        packages[package] |= mask;
        const auto cache_root = cpu / "cache";
        if (!std::filesystem::exists(cache_root)) {
            continue; // The Windows contract permits unreported caches.
        }
        unsigned count = 0;
        for (const auto &entry : std::filesystem::directory_iterator(cache_root)) {
            if (!entry.path().filename().string().starts_with("index")) {
                continue;
            }
            if (++count > 32) {
                throw std::runtime_error("too many CPU cache descriptors");
            }
            const auto &path = entry.path();
            const auto type = read_text(path / "type");
            CacheKind kind;
            if (type == "Unified") {
                kind = CacheKind::unified;
            } else if (type == "Instruction") {
                kind = CacheKind::instruction;
            } else if (type == "Data") {
                kind = CacheKind::data;
            } else {
                throw std::runtime_error("unknown CPU cache type");
            }
            ProcessorCache cache{
                guest_mask(cpu_list(read_text(path / "shared_cpu_list")), result.processor_ids),
                number(read_text(path / "level")),
                number(read_text(path / "ways_of_associativity")),
                number(read_text(path / "coherency_line_size")),
                cache_bytes(read_text(path / "size")),
                kind};
            const auto sets = number(read_text(path / "number_of_sets"));
            if (!sets || !cache.ways) {
                throw std::runtime_error("CPU cache associativity is unavailable");
            }
            cache.fully_associative = sets == 1;
            if (!(cache.mask & mask)) {
                throw std::runtime_error("CPU cache does not include its reporting processor");
            }
            const auto same_cache = [&](const ProcessorCache &other) {
                return other.mask == cache.mask && other.level == cache.level && other.kind == cache.kind;
            };
            const auto prior = std::find_if(result.caches.begin(), result.caches.end(), same_cache);
            if (prior == result.caches.end()) {
                result.caches.push_back(cache);
            } else if (*prior != cache) {
                throw std::runtime_error("inconsistent shared CPU cache descriptors");
            }
        }
    }
    for (const auto &[key, mask] : cores) {
        result.cores.push_back({key.second, mask});
    }
    for (const auto &[id, mask] : packages) {
        result.packages.push_back({id, mask});
    }
    std::uint64_t node_coverage = 0;
    for (const auto id : cpu_list(read_text(root / "node/online"))) {
        const auto mask =
            guest_mask(cpu_list(read_text(root / "node" / ("node" + std::to_string(id)) / "cpulist")),
                       result.processor_ids);
        if (mask & node_coverage) {
            throw std::runtime_error("processor belongs to conflicting NUMA nodes");
        }
        if (mask) {
            result.nodes.push_back({id, mask});
            node_coverage |= mask;
        }
    }
    const auto expected = UINT64_MAX >> (64 - result.processor_ids.size());
    if (node_coverage != expected || cpu_list(read_text(root / "cpu/online")) != online) {
        throw std::runtime_error("incomplete or changing CPU topology snapshot");
    }
    std::sort(result.caches.begin(), result.caches.end(), [](const auto &left, const auto &right) {
        if (left.level != right.level) {
            return left.level < right.level;
        }
        if (left.kind != right.kind) {
            return left.kind < right.kind;
        }
        return left.mask < right.mask;
    });
    return result;
}

ProcessorTopology processor_topology() {
#ifdef __linux__
    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    if (sched_getaffinity(0, sizeof(affinity), &affinity)) {
        throw std::runtime_error("native processor affinity unavailable");
    }
    std::vector<unsigned> allowed;
    for (unsigned id = 0; id < CPU_SETSIZE; ++id) {
        if (CPU_ISSET(id, &affinity)) {
            allowed.push_back(id);
        }
    }
    auto result = read_linux_topology("/sys/devices/system", allowed);
    cpu_set_t after;
    if (sched_getaffinity(0, sizeof(after), &after) || !CPU_EQUAL(&affinity, &after)) {
        throw std::runtime_error("native processor affinity changed during discovery");
    }
    return result;
#else
    throw std::runtime_error("native processor topology adapter is unsupported on this host");
#endif
}
} // namespace ambrosia::host
