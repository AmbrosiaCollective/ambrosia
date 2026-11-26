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

    int release() noexcept {
        const auto result = value_;
        value_ = -1;
        return result;
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
        return {{true, 0, static_cast<std::uint64_t>(value.st_dev), static_cast<std::uint64_t>(value.st_ino)},
                FileLookupError::none};
    }
    if (!S_ISREG(value.st_mode) || value.st_size < 0) {
        return {{}, FileLookupError::unsupported};
    }
    const auto flags = ::fcntl(descriptor, F_GETFL);
    if (flags < 0) {
        return {{}, lookup_error(errno)};
    }
    return {{false, static_cast<std::uint64_t>(value.st_size), static_cast<std::uint64_t>(value.st_dev),
             static_cast<std::uint64_t>(value.st_ino), (flags & O_DIRECT) != 0},
            FileLookupError::none};
}

int walk(int root, std::span<const std::string> components, int final_flags, FileLookupError &error) {
    Descriptor current(open_confined(root, ".", O_PATH | O_DIRECTORY));
    if (current.get() < 0) {
        error = lookup_error(errno);
        return -1;
    }
    for (std::size_t index = 0; index < components.size(); ++index) {
        const auto &component = components[index];
        if (component.empty() || component == "." || component == ".." ||
            component.find('/') != std::string::npos || component.find('\\') != std::string::npos ||
            component.find('\0') != std::string::npos) {
            error = FileLookupError::invalid;
            return -1;
        }
        const bool last = index + 1 == components.size();
        const auto flags = last ? final_flags : O_PATH | O_DIRECTORY;
        auto next = open_confined(current.get(), component.c_str(), flags);
        if (next < 0 && errno == ENOENT) {
            const auto found = case_name(current.get(), component);
            if (found.error != FileLookupError::none) {
                error = found.error == FileLookupError::missing && !last ? FileLookupError::missing_directory
                                                                         : found.error;
                return -1;
            }
            next = open_confined(current.get(), found.name->c_str(), flags);
        }
        if (next < 0) {
            error = lookup_error(errno);
            if (error == FileLookupError::missing && !last) {
                error = FileLookupError::missing_directory;
            }
            return -1;
        }
        current.reset(next);
    }
    return current.release();
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
    FileLookupError error = FileLookupError::none;
    Descriptor descriptor(walk(state_->root.get(), components, O_PATH, error));
    if (descriptor.get() < 0) {
        return {{}, error};
    }
    return metadata(descriptor.get());
#else
    (void)components;
    return {{}, FileLookupError::unsupported};
#endif
}

struct ReadonlyFile::State {
#ifdef __linux__
    Descriptor descriptor;
#endif
};

ReadonlyFile::ReadonlyFile(int descriptor) : state_(std::make_unique<State>()) {
#ifdef __linux__
    state_->descriptor.reset(descriptor);
#else
    (void)descriptor;
#endif
}

ReadonlyFile::~ReadonlyFile() = default;

FileLookup ReadonlyFile::inspect() const {
    NativeErrorScope restore;
#ifdef __linux__
    return metadata(state_->descriptor.get());
#else
    return {{}, FileLookupError::unsupported};
#endif
}

FileOpen ReadonlyDirectory::open(std::span<const std::string> components, bool unbuffered) const {
    NativeErrorScope restore;
#ifdef __linux__
    if (components.empty()) {
        return {nullptr, FileLookupError::denied};
    }
    FileLookupError error = FileLookupError::none;
    // NONBLOCK avoids waiting on a FIFO before fstat can reject non-files.
    Descriptor descriptor(
        walk(state_->root.get(), components, O_RDONLY | O_NONBLOCK | (unbuffered ? O_DIRECT : 0), error));
    if (descriptor.get() < 0) {
        return {nullptr, error};
    }
    const auto found = metadata(descriptor.get());
    if (found.error != FileLookupError::none) {
        return {nullptr, found.error};
    }
    if (found.metadata.directory) {
        return {nullptr, FileLookupError::denied};
    }
    auto file = std::unique_ptr<ReadonlyFile>(new ReadonlyFile(-1));
    file->state_->descriptor.reset(descriptor.release());
    return {std::move(file), FileLookupError::none};
#else
    (void)components;
    (void)unbuffered;
    return {nullptr, FileLookupError::unsupported};
#endif
}
} // namespace ambrosia::host
