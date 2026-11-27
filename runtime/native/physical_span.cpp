#include "physical_span.h"
#include <stdexcept>

namespace ambrosia::services {
namespace {
std::size_t view_bytes(const PhysicalSpan &span) {
    if (!span.size() || span.pages().empty() || span.pages().size() > 16384 ||
        span.first_page_offset() >= physical_page_bytes) {
        throw std::invalid_argument("invalid retained physical span");
    }
    const auto bytes = span.pages().size() * physical_page_bytes;
    if (span.size() > bytes - span.first_page_offset()) {
        throw std::invalid_argument("retained physical span exceeds its pages");
    }
    return bytes;
}
} // namespace

PhysicalSpanView::PhysicalSpanView(const PhysicalSpan &span)
    : span_(span), mapping_(view_bytes(span), host::Access::none, physical_page_bytes) {
    for (std::size_t index = 0; index < span_.pages().size(); ++index) {
        const auto &page = span_.pages()[index];
        if (!page.lease) {
            throw std::invalid_argument("retained physical span contains an unmapped page");
        }
        page.lease->map_into(mapping_, index * physical_page_bytes, host::Access::read);
    }
}

const std::byte *PhysicalSpanView::data() const {
    return mapping_.data() + span_.first_page_offset();
}

std::size_t PhysicalSpanView::size() const {
    return span_.size();
}
} // namespace ambrosia::services
