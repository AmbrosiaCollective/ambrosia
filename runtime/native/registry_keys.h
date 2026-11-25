#pragma once
#include "handle_ids.h"
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace ambrosia::services {
struct KeyResult {
    std::uintptr_t handle = 0;
    std::uint32_t error = 0;
};

// An explicit guest configuration namespace, not a view of host settings.
// Initially only the five ordinary predefined roots exist. define_key is a
// host-side configuration operation, not an implementation of RegCreateKey.
class RegistryKeys {
  public:
    explicit RegistryKeys(std::shared_ptr<HandleIds> ids = std::make_shared<HandleIds>(),
                          std::size_t handle_limit = 4096);
    std::uint32_t define_key(std::uintptr_t root, std::u16string_view path);
    KeyResult open(std::uintptr_t parent, std::u16string_view path, bool legacy);
    std::uint32_t close(std::uintptr_t handle);
    bool owns(std::uintptr_t handle) const;

  private:
    using Key = std::pair<std::uint32_t, std::u16string>;
    static std::uint32_t root_index(std::uintptr_t handle);
    static std::u16string normalize(std::u16string_view path);
    std::shared_ptr<const Key> find(std::uintptr_t handle) const;
    std::shared_ptr<HandleIds> ids_;
    std::size_t handle_limit_;
    mutable std::mutex mutex_;
    std::map<Key, std::shared_ptr<const Key>> keys_;
    std::map<std::uintptr_t, std::shared_ptr<const Key>> handles_;
    std::size_t path_bytes_ = 0;
};
} // namespace ambrosia::services
