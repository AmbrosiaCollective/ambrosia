#pragma once
#include <cstddef>
#include <cstdint>

namespace ambrosia::services {
// Pinned public D3D12_RESOURCE_DESC ABI. Preserve the complete console input;
// backend allocation policy does not rewrite the guest-visible description.
struct ResourceDescription {
    std::uint32_t dimension = 0;
    std::uint64_t alignment = 0;
    std::uint64_t width = 0;
    std::uint32_t height = 0;
    std::uint16_t depth = 0;
    std::uint16_t mips = 0;
    std::uint32_t format = 0;
    std::uint32_t samples = 0;
    std::uint32_t quality = 0;
    std::uint32_t layout = 0;
    std::uint32_t flags = 0;
};

static_assert(sizeof(ResourceDescription) == 56);
static_assert(offsetof(ResourceDescription, alignment) == 8);
static_assert(offsetof(ResourceDescription, width) == 16);
static_assert(offsetof(ResourceDescription, depth) == 28);
static_assert(offsetof(ResourceDescription, flags) == 48);

struct ResourceRange {
    std::size_t begin = 0;
    std::size_t end = 0;
};

static_assert(sizeof(ResourceRange) == 16);
} // namespace ambrosia::services
