#pragma once
#include "import_registry.h"
#include <bit>
#include <stdexcept>

namespace ambrosia::services {
class PointerCodec {
  public:
    explicit PointerCodec(std::uintptr_t secret) : secret_(secret) {
        if (!secret) {
            throw std::invalid_argument("pointer encoding requires a nonzero secret");
        }
    }

    std::uintptr_t encode(std::uintptr_t pointer) const noexcept {
        return std::rotl(pointer ^ secret_, 17);
    }

    std::uintptr_t decode(std::uintptr_t encoded) const noexcept {
        return std::rotr(encoded, 17) ^ secret_;
    }

  private:
    const std::uintptr_t secret_;
};

void register_pointer_services(ImportRegistry &registry);
} // namespace ambrosia::services
