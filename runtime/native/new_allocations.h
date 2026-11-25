#pragma once
#include "import_registry.h"
#include <atomic>

namespace ambrosia::services {
using NewAllocationHandler = std::int32_t(AMBROSIA_GUEST_ABI *)(std::size_t);

class NewAllocations {
  public:
    std::int32_t mode() const noexcept {
        return mode_.load();
    }

    std::int32_t replace_mode(std::int32_t mode) noexcept {
        if (mode != 0 && mode != 1) {
            return -1;
        }
        return mode_.exchange(mode);
    }

    NewAllocationHandler replace_handler(NewAllocationHandler handler) noexcept {
        return handler_.exchange(handler);
    }

    NewAllocationHandler handler() const noexcept {
        return handler_.load();
    }

  private:
    std::atomic<std::int32_t> mode_{0};
    std::atomic<NewAllocationHandler> handler_{nullptr};
};
} // namespace ambrosia::services
