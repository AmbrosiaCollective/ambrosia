#pragma once
#include <cstdint>

namespace ambrosia::services {
struct DescriptorHeapDescription {
    std::uint32_t type = 0;
    std::uint32_t count = 0;
    std::uint32_t flags = 0;
    std::uint32_t node_mask = 0;
};

static_assert(sizeof(DescriptorHeapDescription) == 16);
} // namespace ambrosia::services
