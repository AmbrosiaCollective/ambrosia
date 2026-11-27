#pragma once
#include "host_graphics.h"
#include "physical_span.h"

namespace ambrosia::services {
// Retained CPU physical bytes are authoritative between synchronous operations.
// Callers serialize operations with graphics submission. Future asynchronous
// submission must retain this owner and establish the same upload/writeback order.
class PhysicalBuffer {
  public:
    PhysicalBuffer(host::Graphics &graphics, const PhysicalSpan &span);
    ~PhysicalBuffer();
    PhysicalBuffer(const PhysicalBuffer &) = delete;
    PhysicalBuffer &operator=(const PhysicalBuffer &) = delete;
    void close();
    std::size_t size() const;
    std::byte *cpu_data();
    bool cpu_writable() const;
    bool same_pages(const PhysicalSpan &span) const;
    void upload(std::size_t offset, std::size_t bytes);
    void download(std::size_t offset, std::size_t bytes);
    void copy_to(PhysicalBuffer &destination, std::size_t source_offset, std::size_t destination_offset,
                 std::size_t bytes);

  private:
    host::Graphics &graphics_;
    PhysicalSpan span_;
    PhysicalSpanView view_;
    host::GraphicsBuffer buffer_;
    void check_range(std::size_t offset, std::size_t bytes) const;
};
} // namespace ambrosia::services
