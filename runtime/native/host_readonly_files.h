#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
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
    std::uint64_t device = 0;
    std::uint64_t identity = 0;
    bool unbuffered = false;
};

struct FileLookup {
    FileMetadata metadata;
    FileLookupError error = FileLookupError::none;
};

struct FileTimestamp {
    std::int64_t seconds = 0;
    std::uint32_t nanoseconds = 0;
};

struct FileTimes {
    std::optional<FileTimestamp> creation;
    FileTimestamp access;
    FileTimestamp write;
    FileLookupError error = FileLookupError::none;
};

enum class FileReadError { none, invalid, denied, no_access, unsupported, exhausted, failure };

struct FileRead {
    std::uint32_t bytes = 0;
    FileReadError error = FileReadError::none;
};

class ReadonlyFile {
  public:
    static constexpr std::size_t read_limit = 64u * 1024 * 1024;
    ~ReadonlyFile();
    ReadonlyFile(const ReadonlyFile &) = delete;
    ReadonlyFile &operator=(const ReadonlyFile &) = delete;
    FileLookup inspect() const;
    FileTimes times() const;
    FileRead read(std::uint64_t offset, std::span<std::byte> output) const noexcept;

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
    FileOpen open(std::span<const std::string> components, bool unbuffered = false) const;

  private:
    friend class LocalStorageDirectory;
    // Transfer an owned, confined directory descriptor to the writable host
    // adapter. Ordinary readers never receive a native descriptor capability.
    int open_directory(std::span<const std::string> components, FileLookupError &error) const;
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace ambrosia::host
