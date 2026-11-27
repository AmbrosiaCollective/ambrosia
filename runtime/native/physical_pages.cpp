#include "physical_pages.h"
#include "memory_services.h"
#include <algorithm>
#include <stdexcept>

namespace ambrosia::services {
struct PhysicalPage::State {
    explicit State(host::SharedBacking source) : backing(std::move(source)) {
        const auto pages = backing.size() / physical_page_bytes;
        free_offsets.reserve(pages);
        for (auto index = pages; index != 0; --index) {
            free_offsets.push_back((index - 1) * physical_page_bytes);
        }
    }

    std::size_t take() {
        const std::lock_guard lock(mutex);
        if (discard_failed || free_offsets.empty()) {
            throw std::runtime_error("physical page backing is exhausted or cannot be reused");
        }
        const auto offset = free_offsets.back();
        free_offsets.pop_back();
        return offset;
    }

    void release(std::size_t offset) noexcept {
        const std::lock_guard lock(mutex);
        try {
            backing.discard(offset, physical_page_bytes);
            // Capacity is reserved for every page before any leases exist.
            free_offsets.push_back(offset);
        } catch (...) {
            // Never hand old bytes to another allocation after a discard error.
            discard_failed = true;
        }
    }

    std::size_t available() const {
        const std::lock_guard lock(mutex);
        return discard_failed ? 0 : free_offsets.size();
    }

    host::SharedBacking backing;
    mutable std::mutex mutex;
    std::vector<std::size_t> free_offsets;
    bool discard_failed = false;
};

PhysicalPage::~PhysicalPage() {
    if (offset_ != SIZE_MAX) {
        state_->release(offset_);
    }
}

void PhysicalPage::map_into(host::Mapping &destination, std::size_t offset, host::Access access) const {
    if (offset_ == SIZE_MAX) {
        throw std::runtime_error("physical page has no backing lease");
    }
    state_->backing.map_into(destination, offset, offset_, physical_page_bytes, access);
}

void PhysicalPages::bind(host::SharedBacking backing) {
    const std::lock_guard lock(mutex_);
    const auto bytes = backing.size();
    if (backing_ || !bytes || bytes % physical_page_bytes || bytes > maximum_backing_bytes) {
        throw std::runtime_error("physical backing is already bound or has an invalid page budget");
    }
    auto prepared = std::make_shared<PhysicalPage::State>(std::move(backing));
    free_slots_.reserve(bytes / physical_page_bytes);
    backing_ = std::move(prepared);
}

PhysicalAllocation PhysicalPages::allocate(std::uint64_t attributes, std::uint64_t count) noexcept {
    try {
        const std::lock_guard lock(mutex_);
        // Preserve the measured word without guessing its console/GPU bits.
        if (attributes != 0x280 || (count && !backing_)) {
            return {{}, error_not_supported};
        }
        if (!count) {
            return {};
        }
        const auto slots = free_slots_.size() + physical_slot_limit - next_slot_;
        if (count > backing_->available() || count > slots) {
            return {{}, error_not_enough_memory};
        }
        PhysicalAllocation result;
        result.pages.reserve(static_cast<std::size_t>(count));
        std::map<std::uint64_t, std::shared_ptr<PhysicalPage>> prepared;
        const auto reused = std::min<std::uint64_t>(count, free_slots_.size());
        for (std::uint64_t index = 0; index < count; ++index) {
            const auto slot =
                index < reused ? free_slots_[free_slots_.size() - 1 - index] : next_slot_ + index - reused;
            // Allocate ownership/control metadata before taking a backing slot.
            auto page = std::shared_ptr<PhysicalPage>(new PhysicalPage(backing_));
            page->offset_ = backing_->take();
            const auto token = slot * 16;
            prepared.emplace(token, std::move(page));
            result.pages.push_back(token);
        }
        // Node transfer allocates no storage; the integer comparator cannot throw.
        // No IDs become visible until all metadata and backing leases exist.
        pages_.merge(prepared);
        free_slots_.resize(free_slots_.size() - static_cast<std::size_t>(reused));
        next_slot_ += count - reused;
        return result;
    } catch (...) {
        return {{}, error_not_enough_memory};
    }
}

std::uint32_t PhysicalPages::free(std::span<const std::uint64_t> pages) noexcept {
    try {
        const std::lock_guard lock(mutex_);
        for (const auto token : pages) {
            const auto found = pages_.find(token);
            if (found == pages_.end()) {
                return error_invalid_parameter;
            }
            pages_.erase(found);
            free_slots_.push_back(token / 16);
        }
        return 0;
    } catch (...) {
        return error_not_enough_memory;
    }
}

std::shared_ptr<const PhysicalPage> PhysicalPages::lease(std::uint64_t page) const {
    const std::lock_guard lock(mutex_);
    const auto found = pages_.find(page);
    return found == pages_.end() ? nullptr : found->second;
}

std::size_t PhysicalPages::capacity() const {
    const std::lock_guard lock(mutex_);
    return backing_ ? backing_->backing.size() / physical_page_bytes : 0;
}

std::size_t PhysicalPages::available() const {
    const std::lock_guard lock(mutex_);
    return backing_ ? backing_->available() : 0;
}
} // namespace ambrosia::services
