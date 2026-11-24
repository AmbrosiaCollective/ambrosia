// Host services used by the portable PE core. No guest Windows types here.
#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace ambrosia::host {
// A thread-local guest segment scope. Linux uses GS and leaves native FS TLS
// untouched. Unsupported hosts fail before changing their thread environment.
std::uintptr_t guest_segment_base();

class GuestSegmentScope {
  public:
    explicit GuestSegmentScope(void *base);
    ~GuestSegmentScope();
    GuestSegmentScope(const GuestSegmentScope &) = delete;
    GuestSegmentScope &operator=(const GuestSegmentScope &) = delete;
    GuestSegmentScope(GuestSegmentScope &&) = delete;
    GuestSegmentScope &operator=(GuestSegmentScope &&) = delete;

  private:
    std::uintptr_t saved_;
};

enum class Access { none, read, read_write, read_execute };

class Mapping {
  public:
    explicit Mapping(std::size_t bytes, Access initial = Access::read_write, std::size_t alignment = 0);
    ~Mapping();
    Mapping(const Mapping &) = delete;
    Mapping &operator=(const Mapping &) = delete;
    Mapping(Mapping &&other) noexcept;
    Mapping &operator=(Mapping &&other) noexcept;
    void protect(std::size_t offset, std::size_t bytes, Access access);
    void commit(std::size_t offset, std::size_t bytes, Access access);
    void decommit(std::size_t offset, std::size_t bytes);
    void flush_instructions();

    std::byte *data() const {
        return data_;
    }

    std::size_t size() const {
        return size_;
    }

    static std::size_t page_size();

  private:
    void release() noexcept;
    void require_page_range(std::size_t offset, std::size_t bytes) const;
    std::byte *data_ = nullptr;
    std::size_t size_ = 0;
};

struct Paths {
    std::filesystem::path image;
    std::filesystem::path package;
    std::filesystem::path work;
};

// Resolve host paths without changing cwd or creating directories. Explicit
// work roots override host cache conventions; inputs must already exist.
Paths resolve_paths(const std::filesystem::path &image, const std::filesystem::path &package = {},
                    const std::filesystem::path &work = {});
} // namespace ambrosia::host
