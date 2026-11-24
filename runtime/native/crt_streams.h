#pragma once
#include "import_registry.h"
#include <array>
#include <optional>

namespace ambrosia::services {
// UCRT exposes an opaque, pointer-sized FILE. Host libc FILE is a different ABI.
// These stable identities are recognized by this runtime only; future stream
// operations must resolve them here rather than reinterpret them as host FILEs.
class StandardStreams {
  public:
    StandardStreams() = default;
    StandardStreams(const StandardStreams &) = delete;
    StandardStreams &operator=(const StandardStreams &) = delete;

    void *file(unsigned index) {
        return index < files_.size() ? &files_[index] : nullptr;
    }

    std::optional<unsigned> index(const void *file) const {
        for (unsigned index = 0; index < files_.size(); ++index) {
            if (file == &files_[index]) {
                return index;
            }
        }
        return std::nullopt;
    }

  private:
    struct GuestFile {
        std::uintptr_t placeholder = 0;
    };

    static_assert(sizeof(GuestFile) == 8 && alignof(GuestFile) == 8);
    std::array<GuestFile, 3> files_{};
};

void register_crt_streams(ImportRegistry &registry);
} // namespace ambrosia::services
