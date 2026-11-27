#include "physical_buffer.h"
#include <algorithm>
#include <stdexcept>

namespace ambrosia::services {
namespace {
constexpr std::size_t transfer_chunk_bytes = 1024 * 1024;
}

PhysicalBuffer::PhysicalBuffer(host::Graphics &graphics, const PhysicalSpan &span)
    : graphics_(graphics), span_(span), view_(span, host::Access::read_write) {
    buffer_ = graphics_.create_buffer(span_.size());
    try {
        upload(0, size());
    } catch (...) {
        try {
            close();
        } catch (...) {
            // A dead bounded host session owns its remaining GPU cleanup.
        }
        throw;
    }
}

PhysicalBuffer::~PhysicalBuffer() {
    try {
        close();
    } catch (...) {
        // Destructors cannot acknowledge an expired service. Its bounded
        // lifetime owns the independent GPU allocation, never these CPU pages.
    }
}

void PhysicalBuffer::close() {
    if (buffer_.id) {
        graphics_.destroy_buffer(buffer_.id);
        buffer_ = {};
    }
}

std::size_t PhysicalBuffer::size() const {
    return span_.size();
}

std::byte *PhysicalBuffer::cpu_data() {
    if (!cpu_writable()) {
        throw std::runtime_error("resource CPU map would exceed its physical-window write policy");
    }
    return view_.writable_data();
}

bool PhysicalBuffer::cpu_writable() const {
    return std::all_of(span_.pages().begin(), span_.pages().end(), [](const auto &page) {
        const auto policy = page.protection & 0xff;
        return policy == 1 || policy == 4;
    });
}

bool PhysicalBuffer::same_pages(const PhysicalSpan &span) const {
    if (span.size() != span_.size() || span.first_page_offset() != span_.first_page_offset() ||
        span.pages().size() != span_.pages().size()) {
        return false;
    }
    return std::equal(span.pages().begin(), span.pages().end(), span_.pages().begin(),
                      [](const auto &first, const auto &second) { return first.lease == second.lease; });
}

void PhysicalBuffer::check_range(std::size_t offset, std::size_t bytes) const {
    if (!buffer_.id || offset > size() || bytes > size() - offset) {
        throw std::runtime_error("physical buffer transfer exceeds its live retained span");
    }
}

void PhysicalBuffer::upload(std::size_t offset, std::size_t bytes) {
    check_range(offset, bytes);
    while (bytes) {
        const auto count = std::min(bytes, transfer_chunk_bytes);
        graphics_.upload_buffer(buffer_.id, offset, {view_.data() + offset, count});
        offset += count;
        bytes -= count;
    }
}

void PhysicalBuffer::download(std::size_t offset, std::size_t bytes) {
    check_range(offset, bytes);
    while (bytes) {
        const auto count = std::min(bytes, transfer_chunk_bytes);
        graphics_.download_buffer(buffer_.id, offset, {view_.writable_data() + offset, count});
        offset += count;
        bytes -= count;
    }
}

void PhysicalBuffer::copy_to(PhysicalBuffer &destination, std::size_t source_offset,
                             std::size_t destination_offset, std::size_t bytes) {
    check_range(source_offset, bytes);
    destination.check_range(destination_offset, bytes);
    if (&graphics_ != &destination.graphics_ || !bytes || bytes > transfer_chunk_bytes || source_offset % 4 ||
        destination_offset % 4 || bytes % 4 ||
        (this == &destination && source_offset < destination_offset + bytes &&
         destination_offset < source_offset + bytes)) {
        throw std::runtime_error("physical buffer copy requires owned nonoverlapping aligned bounded ranges");
    }
    // Refresh after any CPU alias writes, including aliases belonging to other
    // resources. Write back before completion so the next operation sees the
    // same physical identity even when it uses a different resource wrapper.
    upload(source_offset, bytes);
    destination.upload(destination_offset, bytes);
    graphics_.copy_buffer(buffer_.id, source_offset, destination.buffer_.id, destination_offset,
                          static_cast<std::uint32_t>(bytes));
    destination.download(destination_offset, bytes);
}
} // namespace ambrosia::services
