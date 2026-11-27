// Owned console ABI storage around the canonical portable command recording IR.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace ambrosia {
class CommandList;
}

namespace ambrosia::services {
// Published console layout facts; see REUSE.md. These do not describe every field.
inline constexpr std::size_t console_list_object_bytes = 0x3000;
inline constexpr std::size_t console_list_room_offset = 0x1000;
inline constexpr std::size_t console_list_write_offset = 0x18;
inline constexpr std::size_t console_list_dispatch_end_offset = 0x28;
inline constexpr std::size_t console_list_end_offset = 0x30;
inline constexpr std::size_t console_cache_barrier_limit = 4096;

struct ConsoleCacheBarrier {
    std::uint32_t caches;
    std::uint32_t engines;
    std::uint64_t address;
    std::uint64_t bytes;
    bool operator==(const ConsoleCacheBarrier &) const = default;
};

struct RecordedCacheBarrier {
    ConsoleCacheBarrier request;
    std::size_t command_index;
};

class GraphicsRecording {
  public:
    explicit GraphicsRecording(const std::uintptr_t *table);
    ~GraphicsRecording();
    GraphicsRecording(const GraphicsRecording &) = delete;
    GraphicsRecording &operator=(const GraphicsRecording &) = delete;
    void *guest_object() const;
    bool bounds_intact() const;
    bool close();
    bool reset();
    bool closed() const;
    bool cache_barrier(const ConsoleCacheBarrier &request);
    std::span<const RecordedCacheBarrier> cache_barriers() const;
    CommandList &command_ir();
    const CommandList &command_ir() const;

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace ambrosia::services
