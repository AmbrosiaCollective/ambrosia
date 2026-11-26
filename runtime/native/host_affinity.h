#pragma once
#include "host_priority.h"
#include <span>
#include <vector>

namespace ambrosia::host {
struct ThreadProcessors {
    std::vector<unsigned> ids;
    SchedulingError error = SchedulingError::none;
};

ThreadProcessors thread_processors(std::uint32_t id);
// A failed verification restores the previous host mask before reporting an
// error. Failure to restore throws so callers cannot continue with false state.
SchedulingError set_thread_processors(std::uint32_t id, std::span<const unsigned> processors);
} // namespace ambrosia::host
