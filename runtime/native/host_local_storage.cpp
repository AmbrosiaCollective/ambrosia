#include "host_local_storage.h"
#include "host.h"
#include <cerrno>
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
    state_->descriptor = ::open(state_->root.c_str(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
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
} // namespace ambrosia::host
