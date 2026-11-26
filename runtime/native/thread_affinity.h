#pragma once
#include "host_affinity.h"
#include <array>
#include <cstddef>

namespace ambrosia::services {
struct GroupAffinity {
    std::uint64_t mask = 0;
    std::uint16_t group = 0;
    std::array<std::uint16_t, 3> reserved{};
};

static_assert(sizeof(GroupAffinity) == 16 && offsetof(GroupAffinity, group) == 8);

// The same dense guest-bit -> native CPU mapping used by ProcessorInformation.
// One guest group only; native CPU IDs are never truncated into a uint64 mask.
class ThreadAffinity {
  public:
    explicit ThreadAffinity(std::span<const unsigned> processors);
    std::uint32_t read(std::uint32_t native_id, GroupAffinity &output) const;
    std::uint32_t write(std::uint32_t native_id, const GroupAffinity &value, GroupAffinity &previous) const;

  private:
    std::span<const unsigned> processors_;
};

void register_affinity_services(class ImportRegistry &registry);
} // namespace ambrosia::services
