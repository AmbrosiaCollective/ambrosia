// Host services used by the portable PE core. No guest Windows types here.
#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace ambrosia::host {
// A nonzero runtime secret from the host's nondeterministic entropy source.
// Fail closed if the installed implementation cannot provide entropy.
std::uintptr_t random_pointer_secret();
// Cryptographic OS entropy; unsupported hosts or source failure throw.
std::uint32_t secure_random_u32();
std::uint64_t filetime_from_unix_ticks(std::int64_t ticks_100ns);
std::uint64_t utc_filetime();
std::uint64_t uptime_from_parts(std::int64_t seconds, std::int64_t nanoseconds);
std::uint64_t uptime_milliseconds();
std::int64_t performance_counter() noexcept;
inline constexpr std::int64_t performance_frequency = 1000000000;
std::uint32_t current_process_id();
std::uint32_t current_thread_id();

struct StackBounds {
    std::uintptr_t lower = 0;
    std::uintptr_t upper = 0;
};

StackBounds current_stack_bounds();

// A thread-local guest segment scope. Linux uses GS and leaves native FS TLS
// untouched. Unsupported hosts fail before changing their thread environment.
std::uintptr_t guest_segment_base();
// New host workers must discard Linux clone inheritance of the caller's GS.
void reset_worker_guest_segment();

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
