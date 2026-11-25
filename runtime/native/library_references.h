#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>

namespace ambrosia::services {
// The graph and service catalog pin their images through process cleanup.
// These are extra LoadLibrary references, distinct from non-owning lookups.
class LibraryReferences {
  public:
    bool retain(std::uintptr_t module);
    bool release(std::uintptr_t module);
    std::size_t count(std::uintptr_t module) const;

  private:
    mutable std::mutex mutex_;
    std::map<std::uintptr_t, std::size_t> references_;
    std::size_t total_ = 0;
};
} // namespace ambrosia::services
