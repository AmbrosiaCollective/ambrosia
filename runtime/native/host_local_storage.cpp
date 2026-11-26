#include "host_local_storage.h"
#include "host.h"
#include <cerrno>
#include <mutex>
#include <stdexcept>
#include <string>
#ifdef __linux__
#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace ambrosia::host {
namespace {
struct RestoreError {
    int saved = errno;

    ~RestoreError() {
        errno = saved;
    }
};
} // namespace

struct LocalStorageDirectory::State {
    std::filesystem::path root;
    std::unique_ptr<const ReadonlyDirectory> directory;
    mutable std::mutex mutex;
#ifdef __linux__
    int descriptor = -1;

    ~State() {
        if (descriptor >= 0) {
            (void)::close(descriptor);
        }
    }
#endif
};

LocalStorageDirectory::LocalStorageDirectory(const std::filesystem::path &root)
    : state_(std::make_unique<State>()) {
    RestoreError restore;
    if (root.empty()) {
        throw std::runtime_error("local storage requires an explicit directory");
    }
    state_->root = std::filesystem::canonical(root);
    if (state_->root != std::filesystem::absolute(root).lexically_normal()) {
        throw std::runtime_error("local storage root must not use symlink aliases");
    }
#ifdef __linux__
    state_->directory = std::make_unique<const ReadonlyDirectory>(state_->root);
    auto lookup_error = FileLookupError::none;
    state_->descriptor = state_->directory->open_directory({}, lookup_error);
    if (state_->descriptor < 0) {
        throw std::runtime_error("local storage directory cannot be pinned");
    }
    // An exclusive, descriptor-relative empty probe verifies the actual mount
    // and permissions. It cannot follow a planted link or truncate an old file.
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        const auto name = ".ambrosia-storage-probe-" + std::to_string(secure_random_u32()) + "-" +
                          std::to_string(secure_random_u32());
        open_how how{};
        how.flags = O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC;
        how.mode = 0600;
        how.resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV;
        const auto probe =
            static_cast<int>(::syscall(SYS_openat2, state_->descriptor, name.c_str(), &how, sizeof(how)));
        if (probe < 0) {
            if (errno == EEXIST) {
                continue;
            }
            throw std::runtime_error("local storage directory is not writable");
        }
        (void)::close(probe);
        if (::unlinkat(state_->descriptor, name.c_str(), 0)) {
            throw std::runtime_error("local storage write probe cannot be removed");
        }
        return;
    }
    throw std::runtime_error("local storage write probe collision budget exceeded");
#else
    throw std::runtime_error("local storage host adapter is unsupported on this platform");
#endif
}

LocalStorageDirectory::~LocalStorageDirectory() = default;

std::filesystem::path LocalStorageDirectory::path() const {
    RestoreError restore;
#ifdef __linux__
    struct stat pinned {};

    struct stat current {};

    if (::fstat(state_->descriptor, &pinned) || ::lstat(state_->root.c_str(), &current) ||
        !S_ISDIR(current.st_mode) || pinned.st_dev != current.st_dev || pinned.st_ino != current.st_ino) {
        throw std::runtime_error("local storage root identity changed");
    }
    if (::syscall(SYS_faccessat2, state_->descriptor, "", W_OK | X_OK, AT_EMPTY_PATH | AT_EACCESS)) {
        throw std::runtime_error("local storage root is no longer writable");
    }
    return state_->root;
#else
    throw std::runtime_error("local storage host adapter is unsupported on this platform");
#endif
}

DirectoryCreateError LocalStorageDirectory::create_directory(const FileMetadata &expected_root,
                                                             std::span<const std::string> components) const {
    RestoreError restore;
#ifdef __linux__
    const std::lock_guard guard(state_->mutex);
    (void)path();

    struct stat identity {};

    if (::fstat(state_->descriptor, &identity) || !expected_root.directory ||
        expected_root.device != static_cast<std::uint64_t>(identity.st_dev) ||
        expected_root.identity != static_cast<std::uint64_t>(identity.st_ino)) {
        return DirectoryCreateError::denied;
    }
    if (components.empty()) {
        return DirectoryCreateError::already_exists;
    }
    if (components.size() > 1024) {
        return DirectoryCreateError::too_long;
    }
    std::size_t bytes = 0;
    for (const auto &component : components) {
        if (component.size() > 131068 - bytes) {
            return DirectoryCreateError::too_long;
        }
        bytes += component.size();
    }
    const auto map_lookup = [](FileLookupError error) {
        switch (error) {
        case FileLookupError::none:
            return DirectoryCreateError::none;
        case FileLookupError::missing:
        case FileLookupError::missing_directory:
            return DirectoryCreateError::missing_parent;
        case FileLookupError::denied:
            return DirectoryCreateError::denied;
        case FileLookupError::invalid:
            return DirectoryCreateError::invalid;
        case FileLookupError::too_long:
            return DirectoryCreateError::too_long;
        case FileLookupError::exhausted:
            return DirectoryCreateError::exhausted;
        case FileLookupError::unsupported:
            return DirectoryCreateError::unsupported;
        case FileLookupError::failure:
            return DirectoryCreateError::failure;
        }
        return DirectoryCreateError::failure;
    };
    // Existing lookup supplies the same component validation and ASCII case
    // policy as asset reads. Only a missing final component permits creation.
    const auto existing = state_->directory->inspect(components);
    if (existing.error == FileLookupError::none) {
        return DirectoryCreateError::already_exists;
    }
    if (existing.error != FileLookupError::missing) {
        return map_lookup(existing.error);
    }
    auto lookup_error = FileLookupError::none;
    const auto parent =
        state_->directory->open_directory(components.first(components.size() - 1), lookup_error);
    if (parent < 0) {
        return map_lookup(lookup_error);
    }
    // No allocation or throwing operation between acquiring and closing parent.
    // mkdirat creates only the final name and never follows a planted final link.
    const auto result = ::mkdirat(parent, components.back().c_str(), 0700);
    const auto native_error = errno;
    (void)::close(parent);
    if (!result) {
        return DirectoryCreateError::none;
    }
    switch (native_error) {
    case EEXIST:
        return DirectoryCreateError::already_exists;
    case ENOENT:
    case ENOTDIR:
        return DirectoryCreateError::missing_parent;
    case EACCES:
    case EPERM:
    case EROFS:
    case ELOOP:
    case EXDEV:
        return DirectoryCreateError::denied;
    case ENAMETOOLONG:
        return DirectoryCreateError::too_long;
    case ENOSPC:
    case EDQUOT:
        return DirectoryCreateError::no_space;
    case ENOMEM:
    case EMFILE:
    case ENFILE:
        return DirectoryCreateError::exhausted;
    default:
        return DirectoryCreateError::failure;
    }
#else
    (void)expected_root;
    (void)components;
    return DirectoryCreateError::unsupported;
#endif
}
} // namespace ambrosia::host
