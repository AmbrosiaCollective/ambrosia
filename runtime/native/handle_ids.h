#pragma once
#include <atomic>
#include <cstdint>
#include <limits>

namespace ambrosia::services {
// Shared by typed object directories. Never reuse an ID after close, expose a
// host pointer/FD, or allocate a predefined registry/pseudo-handle value.
class HandleIds {
  public:
    std::uintptr_t allocate() noexcept {
        auto candidate = next_.load(std::memory_order_relaxed);
        constexpr auto maximum = static_cast<std::uintptr_t>(std::numeric_limits<std::intptr_t>::max());
        while (candidate <= maximum - 4) {
            const auto following = candidate == 0x7ffffffcu ? 0x80000100u : candidate + 4;
            if (next_.compare_exchange_weak(candidate, following, std::memory_order_relaxed)) {
                return candidate;
            }
        }
        return 0;
    }

  private:
    std::atomic<std::uintptr_t> next_{4};
};
} // namespace ambrosia::services
