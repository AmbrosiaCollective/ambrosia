#pragma once
#include "processor_topology.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ambrosia::services {
// Explicit Windows x64 wire layout. Native host structs never cross the ABI.
struct LogicalProcessorInformation {
    std::uint64_t mask = 0;
    std::uint32_t relationship = 0;
    std::uint32_t padding = 0;
    std::array<std::byte, 16> data{};
};

static_assert(sizeof(LogicalProcessorInformation) == 32);
static_assert(offsetof(LogicalProcessorInformation, data) == 16);

class ProcessorInformation {
  public:
    explicit ProcessorInformation(const host::ProcessorTopology &topology);
    std::uint32_t copy(void *buffer, std::uint32_t &bytes) const noexcept;

    std::span<const unsigned> processor_ids() const noexcept {
        return processor_ids_;
    }

  private:
    std::vector<unsigned> processor_ids_;
    std::vector<LogicalProcessorInformation> records_;
};

void register_sysinfo_services(class ImportRegistry &registry);
} // namespace ambrosia::services
