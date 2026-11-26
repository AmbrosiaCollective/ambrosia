#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace ambrosia::host {
enum class FileLookupError {
    none,
    missing,
    missing_directory,
    denied,
    invalid,
    too_long,
    exhausted,
    unsupported,
    failure
};

struct FileMetadata {
    bool directory = false;
    std::uint64_t bytes = 0;
};

struct FileLookup {
    FileMetadata metadata;
    FileLookupError error = FileLookupError::none;
};

class ReadonlyFile {
  public:
    ~ReadonlyFile();
    ReadonlyFile(const ReadonlyFile &) = delete;
    ReadonlyFile &operator=(const ReadonlyFile &) = delete;
    FileLookup inspect() const;

  private:
    friend class ReadonlyDirectory;
    explicit ReadonlyFile(int descriptor);
    struct State;
    std::unique_ptr<State> state_;
};

struct FileOpen {
    std::unique_ptr<ReadonlyFile> file;
    FileLookupError error = FileLookupError::none;
};

// Host-specific descriptor confinement. Components must be relative names, not
// native paths; the host still rejects traversal independently of the frontend.
class ReadonlyDirectory {
  public:
    explicit ReadonlyDirectory(const std::filesystem::path &root);
    ~ReadonlyDirectory();
    ReadonlyDirectory(const ReadonlyDirectory &) = delete;
    ReadonlyDirectory &operator=(const ReadonlyDirectory &) = delete;
    FileLookup inspect(std::span<const std::string> components) const;
    FileOpen open(std::span<const std::string> components) const;

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace ambrosia::host
