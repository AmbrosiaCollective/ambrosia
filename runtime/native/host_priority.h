#pragma once
#include <cstdint>

namespace ambrosia::host {
enum class SchedulingError { none, denied, missing_thread, invalid, unsupported, failure };

struct ThreadNiceness {
    int value = 0;
    SchedulingError error = SchedulingError::none;
};

// Host scheduling adapter only. The caller must validate a live owned thread
// and keep it alive through this operation; no arbitrary guest-supplied TID.
ThreadNiceness thread_niceness(std::uint32_t id) noexcept;
SchedulingError set_thread_niceness(std::uint32_t id, int value) noexcept;
} // namespace ambrosia::host
