// Stable physical backing snapshots, independent of guest virtual-window lifetime.
#pragma once
#include "physical_pages.h"
#include <span>

namespace ambrosia::services {
struct PhysicalSpanPage {
    std::shared_ptr<const PhysicalPage> lease;
    std::uint64_t attributes;
    std::uint32_t protection;
};

class PhysicalSpan {
  public:
    std::size_t size() const {
        return bytes_;
    }

    std::size_t first_page_offset() const {
        return first_page_offset_;
    }

    std::span<const PhysicalSpanPage> pages() const {
        return pages_;
    }

  private:
    friend class MemoryServices;
    std::size_t bytes_ = 0;
    std::size_t first_page_offset_ = 0;
    std::vector<PhysicalSpanPage> pages_;
};

// An optional host CPU view of the snapshot. This is not a GPU resource or a
// claim that the guest's original virtual addresses remain mapped afterward.
class PhysicalSpanView {
  public:
    explicit PhysicalSpanView(const PhysicalSpan &span, host::Access access = host::Access::read);
    const std::byte *data() const;
    std::byte *writable_data();
    std::size_t size() const;

  private:
    // Reverse destruction removes aliases before retiring retained page leases.
    PhysicalSpan span_;
    host::Mapping mapping_;
    host::Access access_;
};
} // namespace ambrosia::services
