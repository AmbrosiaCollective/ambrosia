#pragma once
#include "import_registry.h"
#include <atomic>

namespace ambrosia::services {
class ExceptionFilter {
  public:
    std::uintptr_t replace(std::uintptr_t filter) noexcept {
        return filter_.exchange(filter);
    }

    std::uintptr_t current() const noexcept {
        return filter_.load();
    }

  private:
    // Shared by all guest threads using this Runtime, not by unrelated guest
    // processes or the native host. Registration never dereferences the value.
    std::atomic<std::uintptr_t> filter_{0};
};

void register_exception_services(ImportRegistry &registry);
} // namespace ambrosia::services
