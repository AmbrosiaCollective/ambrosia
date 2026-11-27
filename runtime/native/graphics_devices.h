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

inline constexpr Guid fence_interface{
    0x0a753dcf, 0xc4d8, 0x4b91, {0xad, 0xf6, 0xbe, 0x5a, 0x60, 0xd9, 0x5a, 0x76}};

inline constexpr Guid command_queue_interface{
    0x0ec870a6, 0x5d7e, 0x4c22, {0x8c, 0xfc, 0x5b, 0xaa, 0xe0, 0x76, 0x16, 0xed}};

inline constexpr Guid command_allocator_interface{
    0x6102dee4, 0xaf59, 0x4b09, {0xb9, 0x99, 0xb4, 0x4d, 0x73, 0xf0, 0x9b, 0x24}};

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
