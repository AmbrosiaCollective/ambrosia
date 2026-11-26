#pragma once
#include "host_readonly_files.h"
#include <filesystem>
#include <memory>

namespace ambrosia::host {
enum class DirectoryCreateError {
    none,
    already_exists,
    missing_parent,
    denied,
    invalid,
    too_long,
    no_space,
    exhausted,
    unsupported,
    failure
};

// An explicitly selected writable directory, separate from read-only assets.
// The host adapter pins its identity and verifies real create/remove access.
class LocalStorageDirectory {
  public:
    explicit LocalStorageDirectory(const std::filesystem::path &root);
    ~LocalStorageDirectory();
    LocalStorageDirectory(const LocalStorageDirectory &) = delete;
    LocalStorageDirectory &operator=(const LocalStorageDirectory &) = delete;
    std::filesystem::path path() const;
    DirectoryCreateError create_directory(const FileMetadata &expected_root,
                                          std::span<const std::string> components) const;

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace ambrosia::host
