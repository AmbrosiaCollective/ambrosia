#include "host_readonly_files.h"
#include <cerrno>
#include <optional>
#include <stdexcept>
#include <string_view>
#ifdef __linux__
#include <dirent.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace ambrosia::host {
namespace {
struct NativeErrorScope {
    int saved = errno;

    ~NativeErrorScope() {
        errno = saved;
    }
};
#ifdef __linux__
class Descriptor {
  public:
    explicit Descriptor(int value = -1) : value_(value) {}

    ~Descriptor() {
        if (value_ >= 0) {
            (void)::close(value_);
        }
    }

    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;

    int get() const noexcept {
        return value_;
    }

    void reset(int value) noexcept {
        if (value_ >= 0) {
            (void)::close(value_);
        }
        value_ = value;
    }

  private:
    int value_;
};

FileLookupError lookup_error(int code) {
    switch (code) {
    case ENOENT:
        return FileLookupError::missing;
    case ENOTDIR:
        return FileLookupError::missing_directory;
    case EACCES:
    case EPERM:
    case ELOOP:
    case EXDEV:
        return FileLookupError::denied;
    case EINVAL:
        return FileLookupError::invalid;
    case ENAMETOOLONG:
        return FileLookupError::too_long;
    case EMFILE:
    case ENFILE:
    case ENOMEM:
        return FileLookupError::exhausted;
    case ENOSYS:
    case EOPNOTSUPP:
        return FileLookupError::unsupported;
    default:
        return FileLookupError::failure;
    }
}

int open_confined(int directory, const char *name, int flags) {
    open_how how{};
    how.flags = static_cast<std::uint64_t>(flags | O_CLOEXEC);
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV;
    return static_cast<int>(::syscall(SYS_openat2, directory, name, &how, sizeof(how)));
}

bool equal_ascii_case(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto fold = [](unsigned char unit) {
            return unit >= 'A' && unit <= 'Z' ? unit + ('a' - 'A') : unit;
        };
        if (fold(static_cast<unsigned char>(left[index])) != fold(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }
    return true;
}

struct CaseLookup {
    std::optional<std::string> name;
    FileLookupError error = FileLookupError::none;
};

CaseLookup case_name(int directory, const std::string &requested) {
    Descriptor readable(open_confined(directory, ".", O_RDONLY | O_DIRECTORY));
    if (readable.get() < 0) {
        return {{}, lookup_error(errno)};
    }
    // fdopendir takes ownership. Open a separate description so readdir's
    // offset cannot advance the caller's pinned directory handle.
    const auto duplicate = ::fcntl(readable.get(), F_DUPFD_CLOEXEC, 0);
    if (duplicate < 0) {
        return {{}, lookup_error(errno)};
    }
    auto *stream = ::fdopendir(duplicate);
    if (!stream) {
        const auto error = lookup_error(errno);
        (void)::close(duplicate);
        return {{}, error};
    }

    struct CloseDirectory {
        DIR *stream;

        ~CloseDirectory() {
            (void)::closedir(stream);
        }
    } close{stream};

    std::optional<std::string> found;
    bool ambiguous = false;
    unsigned entries = 0;
    errno = 0;
    while (const auto *entry = ::readdir(stream)) {
        if (++entries > 4096) {
            return {{}, FileLookupError::exhausted};
        }
        const std::string_view name(entry->d_name);
        if (name == requested) {
            return {std::string(name), FileLookupError::none};
        }
        if (equal_ascii_case(name, requested)) {
            ambiguous = found.has_value();
            found = std::string(name);
        }
        errno = 0;
    }
    if (errno) {
        return {{}, lookup_error(errno)};
    }
    if (ambiguous) {
        return {{}, FileLookupError::invalid};
    }
    return {found, found ? FileLookupError::none : FileLookupError::missing};
}

FileLookup metadata(int descriptor) {
    struct stat value {};

    if (::fstat(descriptor, &value)) {
        return {{}, lookup_error(errno)};
    }
    if (S_ISDIR(value.st_mode)) {
        return {{true, 0}, FileLookupError::none};
    }
    if (!S_ISREG(value.st_mode) || value.st_size < 0) {
        return {{}, FileLookupError::unsupported};
    }
    return {{false, static_cast<std::uint64_t>(value.st_size)}, FileLookupError::none};
}
#endif
} // namespace

struct ReadonlyDirectory::State {
#ifdef __linux__
    Descriptor root;

    explicit State(int descriptor) : root(descriptor) {}
#endif
};

ReadonlyDirectory::ReadonlyDirectory(const std::filesystem::path &root) {
    NativeErrorScope restore;
#ifdef __linux__
    const auto descriptor = ::open(root.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        throw std::runtime_error("cannot pin the configured read-only directory");
    }
    Descriptor owned(descriptor);
    Descriptor probe(open_confined(descriptor, ".", O_PATH | O_DIRECTORY));
    if (probe.get() < 0) {
        throw std::runtime_error("confined native directory lookup is unavailable");
    }
    state_ = std::make_unique<State>(-1);
    state_->root.reset(::fcntl(descriptor, F_DUPFD_CLOEXEC, 0));
    if (state_->root.get() < 0) {
        throw std::runtime_error("cannot retain the configured read-only directory");
    }
#else
    (void)root;
    throw std::runtime_error("confined read-only directories are unsupported on this host");
#endif
}

ReadonlyDirectory::~ReadonlyDirectory() = default;

FileLookup ReadonlyDirectory::inspect(std::span<const std::string> components) const {
    NativeErrorScope restore;
#ifdef __linux__
    Descriptor current(open_confined(state_->root.get(), ".", O_PATH | O_DIRECTORY));
    if (current.get() < 0) {
        return {{}, lookup_error(errno)};
    }
    for (std::size_t index = 0; index < components.size(); ++index) {
        const auto &component = components[index];
        if (component.empty() || component == "." || component == ".." ||
            component.find('/') != std::string::npos || component.find('\\') != std::string::npos ||
            component.find('\0') != std::string::npos) {
            return {{}, FileLookupError::invalid};
        }
        const bool last = index + 1 == components.size();
        const auto flags = O_PATH | (last ? 0 : O_DIRECTORY);
        auto next = open_confined(current.get(), component.c_str(), flags);
        if (next < 0 && errno == ENOENT) {
            const auto found = case_name(current.get(), component);
            if (found.error != FileLookupError::none) {
                const auto error = found.error == FileLookupError::missing && !last
                                       ? FileLookupError::missing_directory
                                       : found.error;
                return {{}, error};
            }
            next = open_confined(current.get(), found.name->c_str(), flags);
        }
        if (next < 0) {
            auto error = lookup_error(errno);
            if (error == FileLookupError::missing && !last) {
                error = FileLookupError::missing_directory;
            }
            return {{}, error};
        }
        current.reset(next);
    }
    return metadata(current.get());
#else
    (void)components;
    return {{}, FileLookupError::unsupported};
#endif
}
} // namespace ambrosia::host
