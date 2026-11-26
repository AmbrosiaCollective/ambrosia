#pragma once
#include "runtime_interface.h"
#include <mutex>
#include <vector>

namespace ambrosia::services {
inline constexpr Guid storage_factory_x2_id{
    0x62959d11, 0xd45f, 0x4884, {0xa0, 0xa3, 0xf7, 0x86, 0x2e, 0xa2, 0x43, 0xae}};

struct StorageConfiguration {
    std::uint32_t staging_bytes = 0;
    std::uint32_t debug_flags = 0;
};

// Console factory contract, not the PC DirectStorage vtable. No files, queues
// or requests are published until their native ownership/execution paths exist.
class StorageFactory {
  public:
    static constexpr std::uint32_t staging_limit = 256 * 1024 * 1024;
    StorageFactory();
    std::int32_t query(const void *interface_id, void *output) noexcept;
    std::int32_t set_staging_size(std::uint32_t bytes);
    void set_debug_flags(std::uint32_t flags);
    StorageConfiguration configuration() const;

    bool owns_interface(const void *object) const noexcept {
        return object == &object_;
    }

    std::uint32_t references() const noexcept {
        return object_.references.load();
    }

  private:
    RuntimeInterface object_;
    mutable std::mutex mutex_;
    std::vector<std::byte> staging_;
    std::uint32_t debug_flags_ = 0;
};

void register_storage_factory(ImportRegistry &registry);
} // namespace ambrosia::services
