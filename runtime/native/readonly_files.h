#pragma once
#include "host_readonly_files.h"
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

namespace ambrosia::services {
struct ResolvedFilePath {
    std::shared_ptr<const host::ReadonlyDirectory> directory;
    std::vector<std::string> components;
    bool directory_only = false;
    std::uint32_t error = 0;
};

struct FileAttributes {
    std::uint32_t value = 0xffffffffu;
    std::uint32_t error = 0;
};

struct ReadonlyFileOpen {
    std::unique_ptr<host::ReadonlyFile> file;
    std::uint32_t error = 0;
};

// One read-only asset namespace. Native origins are confined to the configured
// package/image roots; DOS-style volume hints are aliases of the asset root.
class ReadonlyFiles {
  public:
    void bind(const std::filesystem::path &package, const std::filesystem::path &image);
    ResolvedFilePath resolve(std::string_view path) const;
    FileAttributes attributes(std::string_view path) const;
    ReadonlyFileOpen open(std::string_view path) const;

  private:
    struct Roots;
    mutable std::mutex mutex_;
    std::shared_ptr<const Roots> roots_;
};
} // namespace ambrosia::services
