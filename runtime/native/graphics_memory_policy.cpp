// Observed-shape validation adapted from Xbox2Windows d3d12_xs_shim.cpp at
// 481a1c62c2c4306e5b0320ffe811ef2816f5809b, MIT. See LICENSE.Xbox2Windows.
#include "graphics_memory_policy.h"
#include <cstring>

namespace ambrosia::services {
std::int32_t GraphicsMemoryPolicy::configure(std::span<const std::byte> description) {
    constexpr auto invalid_argument = static_cast<std::int32_t>(0x80070057u);
    constexpr auto invalid_call = static_cast<std::int32_t>(0x887a0001u);
    if (description.size() != 32) {
        return invalid_argument;
    }
    std::uint32_t range_class = 0;
    std::memcpy(&range_class, description.data() + 4, sizeof(range_class));
    if (range_class != 3 && range_class != 4) {
        return invalid_argument;
    }
    for (std::size_t index = 0; index < description.size(); ++index) {
        if (description[index] != std::byte{} && (index < 4 || index >= 8)) {
            return invalid_argument;
        }
    }
    const std::lock_guard guard(mutex_);
    if (plan_ || device_created_) {
        return invalid_call;
    }
    // The observed caller computes ceil(range / 16 GiB) + 2. Retain its
    // coverage policy; do not mistake this for a successfully reserved range.
    plan_ = GraphicsMemoryPlan{range_class, std::uint64_t(range_class - 2) * (16ull << 30)};
    return 0;
}

std::optional<GraphicsMemoryPlan> GraphicsMemoryPolicy::snapshot() const {
    const std::lock_guard guard(mutex_);
    return plan_;
}

GraphicsMemoryPolicy::DeviceCreation::DeviceCreation(GraphicsMemoryPolicy &owner)
    : owner_(owner), lock_(owner.mutex_) {}

std::optional<GraphicsMemoryPlan> GraphicsMemoryPolicy::DeviceCreation::plan() const {
    return owner_.plan_;
}

void GraphicsMemoryPolicy::DeviceCreation::succeeded() {
    owner_.device_created_ = true;
}

GraphicsMemoryPolicy::DeviceCreation GraphicsMemoryPolicy::begin_device_creation() {
    return DeviceCreation(*this);
}
} // namespace ambrosia::services
