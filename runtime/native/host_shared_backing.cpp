#include "host_shared_backing.h"
#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <utility>
#ifdef __linux__
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ambrosia::host {
SharedBacking SharedBacking::adopt_descriptor(int descriptor, std::size_t maximum_bytes) {
    if (descriptor < 3) {
        throw std::runtime_error("shared backing requires an explicitly owned descriptor");
    }
#ifdef __linux__
    // The local owner closes even if validation or an allocation below fails.
    SharedBacking owner(descriptor, 0);

    struct stat information {};

    if (fstat(descriptor, &information)) {
        throw std::system_error(errno, std::generic_category(), "cannot inspect shared backing");
    }
    const auto page = Mapping::page_size();
    if (!S_ISREG(information.st_mode) || information.st_nlink != 0 || information.st_size <= 0 ||
        static_cast<std::uint64_t>(information.st_size) > maximum_bytes || information.st_size % page) {
        throw std::runtime_error("shared backing is not a bounded anonymous page file");
    }
    const auto seals = fcntl(descriptor, F_GET_SEALS);
    constexpr auto required_seals = F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
    if (seals < 0 || (seals & required_seals) != required_seals ||
        (seals & (F_SEAL_WRITE | F_SEAL_FUTURE_WRITE))) {
        throw std::runtime_error("shared backing requires immutable size and writable contents");
    }
    const auto flags = fcntl(descriptor, F_GETFL);
    if (flags < 0 || (flags & O_ACCMODE) != O_RDWR) {
        throw std::runtime_error("shared backing requires an owned read-write descriptor");
    }
    owner.bytes_ = static_cast<std::size_t>(information.st_size);
    return owner;
#else
    (void)maximum_bytes;
    throw std::runtime_error("shared backing descriptor adoption is unsupported on this host");
#endif
}

SharedBacking::~SharedBacking() {
    release();
}

void SharedBacking::release() noexcept {
#ifdef __linux__
    if (descriptor_ >= 0) {
        // Do not retry close after EINTR: the descriptor may already be gone.
        (void)close(descriptor_);
    }
#endif
    descriptor_ = -1;
    bytes_ = 0;
}

SharedBacking::SharedBacking(SharedBacking &&other) noexcept
    : descriptor_(std::exchange(other.descriptor_, -1)), bytes_(std::exchange(other.bytes_, 0)) {}

SharedBacking &SharedBacking::operator=(SharedBacking &&other) noexcept {
    if (this != &other) {
        release();
        descriptor_ = std::exchange(other.descriptor_, -1);
        bytes_ = std::exchange(other.bytes_, 0);
    }
    return *this;
}

void SharedBacking::require_range(std::size_t offset, std::size_t bytes) const {
    const auto page = Mapping::page_size();
    if (descriptor_ < 0 || !bytes || offset % page || bytes % page || offset > bytes_ ||
        bytes > bytes_ - offset) {
        throw std::runtime_error("invalid shared backing page range");
    }
}

void SharedBacking::map_into(Mapping &destination, std::size_t destination_offset, std::size_t backing_offset,
                             std::size_t bytes, Access access) const {
    require_range(backing_offset, bytes);
    const auto page = Mapping::page_size();
    if (!destination.data() || destination_offset % page || destination_offset > destination.size() ||
        bytes > destination.size() - destination_offset ||
        (access != Access::read && access != Access::read_write)) {
        throw std::runtime_error("invalid shared backing destination or access");
    }
#ifdef __linux__
    auto *target = destination.data() + destination_offset;
    const auto protection = PROT_READ | (access == Access::read_write ? PROT_WRITE : 0);
    const auto result = mmap(target, bytes, protection, MAP_SHARED | MAP_FIXED, descriptor_,
                             static_cast<off_t>(backing_offset));
    if (result == MAP_FAILED) {
        const auto error = errno;
        // A failed replacement must leave a reservation, not a reusable hole.
        destination.decommit(destination_offset, bytes);
        throw std::system_error(error, std::generic_category(), "cannot map owned shared backing");
    }
#else
    throw std::runtime_error("shared backing mapping is unsupported on this host");
#endif
}

void SharedBacking::discard(std::size_t offset, std::size_t bytes) const {
    require_range(offset, bytes);
#ifdef __linux__
    if (fallocate(descriptor_, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, static_cast<off_t>(offset),
                  static_cast<off_t>(bytes))) {
        throw std::system_error(errno, std::generic_category(), "cannot discard retired backing pages");
    }
#else
    throw std::runtime_error("shared backing discard is unsupported on this host");
#endif
}
} // namespace ambrosia::host
