#pragma once
#include "readonly_files.h"
#include "runtime_interface.h"
#include <memory>
#include <mutex>
#include <vector>

namespace ambrosia::services {
inline constexpr Guid storage_factory_x2_id{
    0x62959d11, 0xd45f, 0x4884, {0xa0, 0xa3, 0xf7, 0x86, 0x2e, 0xa2, 0x43, 0xae}};
inline constexpr Guid storage_file_x_id{
    0x43704f9f, 0x27c6, 0x4a9f, {0x88, 0x15, 0xd1, 0x65, 0x27, 0x89, 0x4d, 0xf4}};

struct StorageConfiguration {
    std::uint32_t staging_bytes = 0;
    std::uint32_t debug_flags = 0;
};

// Console factory contract, not the PC DirectStorage vtable. Files own real
// read-only descriptors; request queues remain unsupported.
class StorageFactory {
  public:
    static constexpr std::uint32_t staging_limit = 256 * 1024 * 1024;
    StorageFactory();
    ~StorageFactory();
    std::int32_t query(const void *interface_id, void *output) noexcept;
    std::int32_t set_staging_size(std::uint32_t bytes);
    void set_debug_flags(std::uint32_t flags);
    StorageConfiguration configuration() const;
    std::int32_t open_file(ReadonlyFiles &files, std::string_view path, const void *interface_id,
                           void *output);
    std::int32_t query_file(void *self, const void *interface_id, void *output);
    std::uint32_t add_file_reference(void *self);
    std::uint32_t release_file(void *self);
    void close_file(void *self);
    host::FileLookup inspect_file(void *self) const;

    bool owns_interface(const void *object) const noexcept {
        return object == &object_;
    }

    std::uint32_t references() const noexcept {
        return object_.references.load();
    }

  private:
    struct File;
    File &file(void *self) const;
    RuntimeInterface object_;
    mutable std::mutex mutex_;
    std::vector<std::byte> staging_;
    std::uint32_t debug_flags_ = 0;
    // Keep bounded tombstones until runtime teardown so stale guest pointers
    // can never name a different newly opened file. Final release closes the FD.
    std::vector<std::unique_ptr<File>> files_;
};

void register_storage_factory(ImportRegistry &registry);
} // namespace ambrosia::services
