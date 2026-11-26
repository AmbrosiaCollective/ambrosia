#include "processor_information.h"
#include <bit>
#include <cstring>
#include <set>
#include <stdexcept>

namespace ambrosia::services {
namespace {
template <class Value> void store(LogicalProcessorInformation &record, std::size_t offset, Value value) {
    std::memcpy(record.data.data() + offset, &value, sizeof(value));
}
} // namespace

ProcessorInformation::ProcessorInformation(const host::ProcessorTopology &topology) {
    const auto count = topology.processor_ids.size();
    if (!count || count > 64 || topology.caches.size() > 2048 ||
        std::set<unsigned>(topology.processor_ids.begin(), topology.processor_ids.end()).size() != count) {
        throw std::runtime_error("invalid or unsupported native processor group");
    }
    processor_ids_ = topology.processor_ids;
    const auto active = UINT64_MAX >> (64 - count);
    const auto add_groups = [&](const std::vector<host::ProcessorGroup> &groups, std::uint32_t relation) {
        std::uint64_t covered = 0;
        if (groups.size() > count) {
            throw std::runtime_error("too many processor relationship groups");
        }
        std::set<std::uint32_t> node_ids;
        for (const auto &group : groups) {
            if (!group.mask || (group.mask & ~active) || (group.mask & covered) ||
                (relation == 1 && !node_ids.insert(group.id).second)) {
                throw std::runtime_error("invalid processor relationship masks or node IDs");
            }
            covered |= group.mask;
            LogicalProcessorInformation record;
            record.mask = group.mask;
            record.relationship = relation;
            if (relation == 0) {
                record.data[0] = std::byte(std::popcount(group.mask) > 1 ? 1 : 0);
            } else if (relation == 1) {
                store(record, 0, group.id);
            }
            records_.push_back(record);
        }
        if (covered != active) {
            throw std::runtime_error("incomplete processor relationship coverage");
        }
    };
    add_groups(topology.cores, 0);
    add_groups(topology.nodes, 1);
    add_groups(topology.packages, 3);
    for (const auto &cache : topology.caches) {
        if (!cache.mask || (cache.mask & ~active) || !cache.level || cache.level > 255 ||
            (!cache.fully_associative && (!cache.ways || cache.ways >= 255)) || !cache.line_bytes ||
            cache.line_bytes > 65535 || !cache.bytes) {
            throw std::runtime_error("CPU cache descriptor does not fit the x64 ABI");
        }
        LogicalProcessorInformation record;
        record.mask = cache.mask;
        record.relationship = 2;
        record.data[0] = std::byte(cache.level);
        record.data[1] = std::byte(cache.fully_associative ? 255 : cache.ways);
        store(record, 2, static_cast<std::uint16_t>(cache.line_bytes));
        store(record, 4, cache.bytes);
        std::uint32_t type;
        switch (cache.kind) {
        case host::CacheKind::unified:
            type = 0;
            break;
        case host::CacheKind::instruction:
            type = 1;
            break;
        case host::CacheKind::data:
            type = 2;
            break;
        default:
            throw std::runtime_error("unsupported CPU cache kind");
        }
        store(record, 8, type);
        records_.push_back(record);
    }
}

std::uint32_t ProcessorInformation::copy(void *buffer, std::uint32_t &bytes) const noexcept {
    const auto required = static_cast<std::uint32_t>(records_.size() * sizeof(LogicalProcessorInformation));
    const auto capacity = bytes;
    bytes = required;
    if (!buffer || capacity < required) {
        return 122; // ERROR_INSUFFICIENT_BUFFER; no partial records are written.
    }
    std::memcpy(buffer, records_.data(), required);
    return 0;
}
} // namespace ambrosia::services
