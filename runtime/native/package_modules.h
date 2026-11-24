#pragma once
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ambrosia::host {
struct PackageLimits {
    std::size_t file_bytes = 64 * 1024 * 1024;
    std::size_t total_bytes = 64 * 1024 * 1024;
    std::size_t directory_entries = 4096;
    std::size_t modules = 64;
};

std::vector<std::byte> read_bootstrap_file(const std::filesystem::path &path, std::size_t limit);

// Index only the explicit package root. Content is read on demand, without
// recursive asset scans, system-library searches or package writes.
class PackageModules {
  public:
    explicit PackageModules(const std::filesystem::path &root, PackageLimits limits = {});
    std::optional<std::vector<std::byte>> read(const std::string &module);

    std::size_t files_read() const {
        return files_read_;
    }

    std::size_t bytes_read() const {
        return bytes_read_;
    }

  private:
    std::map<std::string, std::filesystem::path> paths_;
    PackageLimits limits_;
    std::size_t files_read_ = 0;
    std::size_t bytes_read_ = 0;
};
} // namespace ambrosia::host
