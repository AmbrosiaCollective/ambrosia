#pragma once
#include "host.h"

namespace ambrosia::host {
// An owned, fixed-size anonymous file for CPU page aliases. It represents
// backing bytes, not host physical frame numbers or a GPU allocation.
class SharedBacking {
  public:
    // Linux takes ownership of a descriptor >= 3, even on validation failure,
    // and requires an unlinked regular file with immutable-size seals.
    // Unsupported hosts reject the operation without taking ownership.
    static SharedBacking adopt_descriptor(int descriptor, std::size_t maximum_bytes);
    ~SharedBacking();
    SharedBacking(const SharedBacking &) = delete;
    SharedBacking &operator=(const SharedBacking &) = delete;
    SharedBacking(SharedBacking &&other) noexcept;
    SharedBacking &operator=(SharedBacking &&other) noexcept;

    std::size_t size() const {
        return bytes_;
    }

    // Replace only a checked range of the destination's existing reservation.
    // The caller serializes overlapping replacements and owns any guest leases.
    void map_into(Mapping &destination, std::size_t destination_offset, std::size_t backing_offset,
                  std::size_t bytes, Access access) const;
    // Release backing storage for reuse. The caller must first retire every
    // page lease and alias for this range; subsequent reads then observe zeros.
    void discard(std::size_t offset, std::size_t bytes) const;

  private:
    SharedBacking(int descriptor, std::size_t bytes) : descriptor_(descriptor), bytes_(bytes) {}

    void release() noexcept;
    void require_range(std::size_t offset, std::size_t bytes) const;
    int descriptor_ = -1;
    std::size_t bytes_ = 0;
};
} // namespace ambrosia::host
