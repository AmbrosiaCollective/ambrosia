#pragma once
#include "graphics_memory_policy.h"
#include "runtime_interface.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

namespace ambrosia::services {
// Public ID3D12Device11 identity from pinned DirectX-Headers61c7718.
inline constexpr Guid device11_interface{
    0x5405c344, 0xd457, 0x444e, {0xb4, 0xdd, 0x23, 0x66, 0xe4, 0x5a, 0xee, 0x39}};

class GraphicsDevices {
  public:
    GraphicsDevices();
    ~GraphicsDevices();
    GraphicsDevices(const GraphicsDevices &) = delete;
    GraphicsDevices &operator=(const GraphicsDevices &) = delete;
    void bind(const std::filesystem::path &endpoint_root);
    bool configured() const;
    std::int32_t create(const void *interface_id, void **output,
                        std::optional<GraphicsMemoryPlan> memory_plan);

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace ambrosia::services
