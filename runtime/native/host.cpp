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
    if (size <= 0) throw std::runtime_error("cannot determine host page size");
    return static_cast<std::size_t>(size);
#endif
}

Mapping::Mapping(std::size_t bytes) {
    const auto page = page_size();
    if (!bytes || bytes > std::numeric_limits<std::size_t>::max() - (page - 1))
        throw std::runtime_error("invalid mapping size");
    size_ = (bytes + page - 1) / page * page;
#ifdef _WIN32
    data_ = static_cast<std::byte*>(VirtualAlloc(nullptr, size_, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!data_) throw std::runtime_error("host memory allocation failed");
#else
    void* memory = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) throw std::runtime_error("host memory allocation failed");
    data_ = static_cast<std::byte*>(memory);
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
Mapping::~Mapping() { release(); }
Mapping::Mapping(Mapping&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)) {}
Mapping& Mapping::operator=(Mapping&& other) noexcept {
    if (this != &other) {
        release();
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}
void Mapping::protect(std::size_t offset, std::size_t bytes, Access access) {
    const auto page = page_size();
    if (!data_ || !bytes || offset % page || bytes % page || offset > size_ || bytes > size_ - offset)
        throw std::runtime_error("invalid host protection range");
#ifdef _WIN32
    DWORD protection = PAGE_NOACCESS;
    if (access == Access::read) protection = PAGE_READONLY;
    if (access == Access::read_write) protection = PAGE_READWRITE;
    if (access == Access::read_execute) protection = PAGE_EXECUTE_READ;
    DWORD previous;
    if (!VirtualProtect(data_ + offset, bytes, protection, &previous))
        throw std::runtime_error("host protection failed");
#else
    int protection = PROT_NONE;
    if (access == Access::read) protection = PROT_READ;
    if (access == Access::read_write) protection = PROT_READ | PROT_WRITE;
    if (access == Access::read_execute) protection = PROT_READ | PROT_EXEC;
    if (mprotect(data_ + offset, bytes, protection)) throw std::runtime_error("host protection failed");
#endif
}
void Mapping::flush_instructions() {
#ifdef _WIN32
    if (!FlushInstructionCache(GetCurrentProcess(), data_, size_))
        throw std::runtime_error("instruction cache flush failed");
#else
    __builtin___clear_cache(reinterpret_cast<char*>(data_), reinterpret_cast<char*>(data_ + size_));
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
