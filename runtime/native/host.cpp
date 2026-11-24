#include "host.h"
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace ambrosia::host {
std::size_t Mapping::page_size() {
#ifdef _WIN32
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    return info.dwPageSize;
#else
    const auto size = sysconf(_SC_PAGESIZE);
    if (size <= 0) {
        throw std::runtime_error("cannot determine host page size");
    }
    return static_cast<std::size_t>(size);
#endif
}

namespace {
#ifdef _WIN32
DWORD native_access(Access access) {
    switch (access) {
    case Access::none:
        return PAGE_NOACCESS;
    case Access::read:
        return PAGE_READONLY;
    case Access::read_write:
        return PAGE_READWRITE;
    case Access::read_execute:
        return PAGE_EXECUTE_READ;
    }
    throw std::runtime_error("invalid host access mode");
}
#else
int native_access(Access access) {
    switch (access) {
    case Access::none:
        return PROT_NONE;
    case Access::read:
        return PROT_READ;
    case Access::read_write:
        return PROT_READ | PROT_WRITE;
    case Access::read_execute:
        return PROT_READ | PROT_EXEC;
    }
    throw std::runtime_error("invalid host access mode");
}
#endif
} // namespace

Mapping::Mapping(std::size_t bytes, Access initial, std::size_t alignment) {
    const auto page = page_size();
    if (!alignment) {
        alignment = page;
    }
    if (!bytes || bytes > std::numeric_limits<std::size_t>::max() - (page - 1) || alignment < page ||
        alignment % page || (alignment & (alignment - 1))) {
        throw std::runtime_error("invalid mapping size or alignment");
    }
    size_ = (bytes + page - 1) / page * page;
#ifdef _WIN32
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    if (alignment > info.dwAllocationGranularity) {
        throw std::runtime_error("requested host alignment is unsupported");
    }
    const auto flags = MEM_RESERVE | (initial == Access::none ? 0 : MEM_COMMIT);
    data_ = static_cast<std::byte *>(VirtualAlloc(nullptr, size_, flags, native_access(initial)));
    if (!data_) {
        throw std::runtime_error("host memory allocation failed");
    }
#else
    if (size_ > std::numeric_limits<std::size_t>::max() - (alignment - page)) {
        throw std::runtime_error("aligned mapping size overflows");
    }
    const auto reserved_size = size_ + alignment - page;
    void *memory = mmap(nullptr, reserved_size, native_access(initial), MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) {
        throw std::runtime_error("host memory allocation failed");
    }
    const auto address = reinterpret_cast<std::uintptr_t>(memory);
    const auto padding = (alignment - address % alignment) % alignment;
    auto *aligned = static_cast<std::byte *>(memory) + padding;
    if (padding && munmap(memory, padding)) {
        munmap(memory, reserved_size);
        throw std::runtime_error("cannot trim aligned host reservation");
    }
    const auto suffix = reserved_size - padding - size_;
    if (suffix && munmap(aligned + size_, suffix)) {
        munmap(aligned, size_ + suffix);
        throw std::runtime_error("cannot trim aligned host reservation");
    }
    data_ = aligned;
#endif
}

void Mapping::release() noexcept {
    if (data_) {
#ifdef _WIN32
        VirtualFree(data_, 0, MEM_RELEASE);
#else
        munmap(data_, size_);
#endif
    }
    data_ = nullptr;
    size_ = 0;
}

Mapping::~Mapping() {
    release();
}

Mapping::Mapping(Mapping &&other) noexcept
    : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)) {}

Mapping &Mapping::operator=(Mapping &&other) noexcept {
    if (this != &other) {
        release();
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

void Mapping::require_page_range(std::size_t offset, std::size_t bytes) const {
    const auto page = page_size();
    if (!data_ || !bytes || offset % page || bytes % page || offset > size_ || bytes > size_ - offset) {
        throw std::runtime_error("invalid host page range");
    }
}

void Mapping::protect(std::size_t offset, std::size_t bytes, Access access) {
    require_page_range(offset, bytes);
#ifdef _WIN32
    DWORD previous;
    if (!VirtualProtect(data_ + offset, bytes, native_access(access), &previous)) {
        throw std::runtime_error("host protection failed");
    }
#else
    if (mprotect(data_ + offset, bytes, native_access(access))) {
        throw std::runtime_error("host protection failed");
    }
#endif
}

void Mapping::commit(std::size_t offset, std::size_t bytes, Access access) {
    require_page_range(offset, bytes);
#ifdef _WIN32
    if (!VirtualAlloc(data_ + offset, bytes, MEM_COMMIT, native_access(access))) {
        throw std::runtime_error("host commit failed");
    }
#else
    protect(offset, bytes, access);
#endif
}

void Mapping::decommit(std::size_t offset, std::size_t bytes) {
    require_page_range(offset, bytes);
#ifdef _WIN32
    if (!VirtualFree(data_ + offset, bytes, MEM_DECOMMIT)) {
        throw std::runtime_error("host decommit failed");
    }
#else
    // Replace only this mapping's owned pages. Fresh anonymous backing ensures
    // recommit observes zeros, without relying on platform-specific madvise behavior.
    void *result = mmap(data_ + offset, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (result == MAP_FAILED) {
        throw std::runtime_error("host decommit failed");
    }
#endif
}

void Mapping::flush_instructions() {
#ifdef _WIN32
    if (!FlushInstructionCache(GetCurrentProcess(), data_, size_)) {
        throw std::runtime_error("instruction cache flush failed");
    }
#else
    __builtin___clear_cache(reinterpret_cast<char *>(data_), reinterpret_cast<char *>(data_ + size_));
#endif
}

namespace {
std::filesystem::path environment_path(const char* key) {
#ifdef _WIN32
    // Preserve Unicode cache roots rather than interpreting the ANSI code page.
    const wchar_t* wide_key = nullptr;
    if (std::string_view(key) == "LOCALAPPDATA") wide_key = L"LOCALAPPDATA";
    if (std::string_view(key) == "HOME") wide_key = L"HOME";
    if (!wide_key) return {};
    const auto* value = _wgetenv(wide_key);
#else
    const auto* value = std::getenv(key);
#endif
    return value && *value ? std::filesystem::path(value) : std::filesystem::path{};
}
std::filesystem::path cache_root() {
#ifdef _WIN32
    auto root = environment_path("LOCALAPPDATA");
#elif defined(__APPLE__)
    auto root = environment_path("HOME");
    if (!root.empty()) root /= "Library/Caches";
#else
    auto root = environment_path("XDG_CACHE_HOME");
    if (!root.is_absolute()) root.clear();
    if (root.empty()) {
        root = environment_path("HOME");
        if (!root.empty()) root /= ".cache";
    }
#endif
    if (!root.is_absolute()) root = std::filesystem::temp_directory_path();
    return root / "ambrosia";
}
bool within(const std::filesystem::path& child, const std::filesystem::path& parent) {
    auto a = child.begin(), b = parent.begin();
    for (; b != parent.end(); ++a, ++b)
        if (a == child.end() || *a != *b) return false;
    return true;
}
}

Paths resolve_paths(const std::filesystem::path& image, const std::filesystem::path& package,
                    const std::filesystem::path& work) {
    if (image.empty() || !std::filesystem::is_regular_file(image))
        throw std::runtime_error("image must name an existing regular file");
    Paths paths;
    paths.image = std::filesystem::canonical(image);
    paths.package = std::filesystem::canonical(package.empty() ? paths.image.parent_path() : package);
    if (!std::filesystem::is_directory(paths.package)) throw std::runtime_error("package must be a directory");
    paths.work = std::filesystem::weakly_canonical(std::filesystem::absolute(work.empty() ? cache_root() : work));
    if (within(paths.work, paths.package) || within(paths.package, paths.work) ||
        within(paths.image, paths.work))
        throw std::runtime_error("work root must be separate from package and executable");
    return paths;
}
} // namespace ambrosia::host
