#pragma once
#include "import_registry.h"
#include <atomic>

namespace ambrosia::services {
using InvalidParameterHandler = void(AMBROSIA_GUEST_ABI *)(const char16_t *expression,
                                                           const char16_t *function, const char16_t *file,
                                                           std::uint32_t line, std::uintptr_t reserved);

class InvalidParameters {
  public:
    InvalidParameterHandler replace(InvalidParameterHandler handler) noexcept {
        return handler_.exchange(handler);
    }

    InvalidParameterHandler handler() const noexcept {
        return handler_.load();
    }

  private:
    std::atomic<InvalidParameterHandler> handler_{nullptr};
};

// Dispatch requires active guest TLS. A returning handler leaves error/result
// handling to the calling CRT API. The default fatal Watson path stops explicitly.
void dispatch_invalid_parameter(const char16_t *expression = nullptr, const char16_t *function = nullptr,
                                const char16_t *file = nullptr, std::uint32_t line = 0,
                                std::uintptr_t reserved = 0) noexcept;
void register_invalid_parameters(ImportRegistry &registry);
} // namespace ambrosia::services
