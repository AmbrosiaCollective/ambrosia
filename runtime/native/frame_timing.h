// Native timer-grid policy; source adaptation and limits are recorded in REUSE.md.
#pragma once
#include <array>
#include <cstdint>

namespace ambrosia::services {
struct FrameConfiguration {
    std::uint32_t period_us = 0;
    std::uint32_t latency = 0;
    std::uint64_t epoch_us = 0;
    bool operator==(const FrameConfiguration &) const = default;
};

struct FrameWait {
    std::int32_t result = static_cast<std::int32_t>(0x80070057u);
    std::uint64_t wake_us = 0;
    std::uint64_t token = 0;
};

// The owner serializes access. Planning uses explicit monotonic time and returns
// a value snapshot so a host wait never holds the graphics ownership mutex.
class FrameTiming {
  public:
    bool set_interval(std::uint32_t period_us, std::uint32_t latency, std::uint64_t now_us);
    bool schedule(std::uint32_t event, std::uint32_t offset_us);
    FrameWait prepare_wait(std::uint32_t event, std::uint32_t timeout_ms, std::uint32_t flags,
                           std::uint64_t token, std::uint64_t now_us) const;
    const FrameConfiguration &configuration() const;

  private:
    FrameConfiguration configuration_;
    std::array<std::uint32_t, 4> offsets_{};
    std::array<bool, 4> scheduled_{};
};
} // namespace ambrosia::services
