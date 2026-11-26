#pragma once
#include <filesystem>
#include <memory>

namespace ambrosia::host {
// An explicitly selected writable directory, separate from read-only assets.
// The host adapter pins its identity and verifies real create/remove access.
class LocalStorageDirectory {
  public:
    explicit LocalStorageDirectory(const std::filesystem::path &root);
    ~LocalStorageDirectory();
    LocalStorageDirectory(const LocalStorageDirectory &) = delete;
    LocalStorageDirectory &operator=(const LocalStorageDirectory &) = delete;
    std::filesystem::path path() const;

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace ambrosia::host
