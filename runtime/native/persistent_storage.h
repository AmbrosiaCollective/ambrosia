#pragma once
#include "host_local_storage.h"
#include "readonly_files.h"
#include "runtime_interface.h"
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace ambrosia::services {
inline constexpr Guid persistent_storage_class_id{
    0xf4faf4d4, 0x2d04, 0x4fce, {0xb3, 0xe0, 0x47, 0x4a, 0x71, 0x3a, 0x3e, 0x84}};
inline constexpr Guid persistent_storage_interface_2{
    0xd29411df, 0x0794, 0x4553, {0x8b, 0x27, 0x95, 0xfc, 0x02, 0xd0, 0xf7, 0x5d}};
inline constexpr Guid persistent_storage_interface_3{
    0x41a4e10c, 0x5a7e, 0x41d9, {0x8c, 0x37, 0x37, 0xbd, 0xe6, 0x2a, 0x07, 0xd6}};

class PersistentStorage {
  public:
    PersistentStorage();
    void bind(const std::filesystem::path &root);
    std::optional<std::string> path() const;
    std::uint32_t create_directory(const ResolvedFilePath &path) const;
    std::int32_t query(const Guid &interface_id, void *output) noexcept;

  private:
    mutable std::mutex mutex_;
    std::shared_ptr<const host::LocalStorageDirectory> directory_;
    RuntimeInterface object_;
};
} // namespace ambrosia::services
