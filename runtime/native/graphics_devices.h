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
// Guest record strides. Host Vulkan descriptors have their own representation.
inline constexpr std::array<std::uint32_t, 5> guest_descriptor_increments{32, 16, 32, 32, 32};
inline constexpr Guid resource_interface{
    0x696442be, 0xa72e, 0x4059, {0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad}};
// Public ID3D12Device11 identity from pinned DirectX-Headers61c7718.
inline constexpr Guid device11_interface{
    0x5405c344, 0xd457, 0x444e, {0xb4, 0xdd, 0x23, 0x66, 0xe4, 0x5a, 0xee, 0x39}};

inline constexpr Guid fence_interface{
    0x0a753dcf, 0xc4d8, 0x4b91, {0xad, 0xf6, 0xbe, 0x5a, 0x60, 0xd9, 0x5a, 0x76}};

inline constexpr Guid command_queue_interface{
    0x0ec870a6, 0x5d7e, 0x4c22, {0x8c, 0xfc, 0x5b, 0xaa, 0xe0, 0x76, 0x16, 0xed}};

inline constexpr Guid command_allocator_interface{
    0x6102dee4, 0xaf59, 0x4b09, {0xb9, 0x99, 0xb4, 0x4d, 0x73, 0xf0, 0x9b, 0x24}};

inline constexpr Guid command_list_interface{
    0x7116d91c, 0xe7e4, 0x47ce, {0xb8, 0xc6, 0xec, 0x81, 0x68, 0xf4, 0x37, 0xe5}};

inline constexpr Guid graphics_command_list_interface{
    0x5b160d0f, 0xac1b, 0x4185, {0x8b, 0xa8, 0xb3, 0xae, 0x42, 0xa5, 0xa4, 0x55}};

// Console GCL6 identity published by FiveStar; the source pin is recorded in REUSE.md.
inline constexpr Guid console_command_list_interface{
    0xc3827890, 0x4024, 0x474c, {0x98, 0xcf, 0x64, 0x5b, 0x51, 0xdc, 0x48, 0x74}};

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
