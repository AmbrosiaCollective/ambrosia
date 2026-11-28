#include "memory_services.h"
#include "physical_pages.h"
#include <algorithm>
#include <exception>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace ambrosia::services {
namespace {
constexpr std::uint32_t commit = 0x1000;
constexpr std::uint32_t reserve = 0x2000;
constexpr std::uint32_t decommit = 0x4000;
constexpr std::uint32_t release = 0x8000;
constexpr std::uint32_t preserve_placeholder = 2;

// Adapted from the inherited xmem.cpp::round_up_size at fca8458.
bool round_up(std::size_t value, std::size_t alignment, std::size_t &rounded) {
    if (!alignment || (alignment & (alignment - 1)) ||
        value > std::numeric_limits<std::size_t>::max() - (alignment - 1)) {
        return false;
    }
    rounded = (value + alignment - 1) & ~(alignment - 1);
    return true;
}

host::Access access_for(std::uint32_t protection) {
    switch (protection) {
    case 0x01:
        return host::Access::none;
    case 0x02:
        return host::Access::read;
    case 0x04:
        return host::Access::read_write;
    case 0x20:
        return host::Access::read_execute;
    default:
        throw std::runtime_error("unsupported guest memory protection");
    }
}
} // namespace

std::optional<XmemRegionInfo> MemoryServices::Region::window_at(std::uintptr_t pointer) const {
    std::optional<XmemRegionInfo> result;
    if (xmem && xmem->backing == XmemBacking::physical_window) {
        result = xmem;
    } else {
        auto child = placeholder_windows.upper_bound(pointer);
        if (child != placeholder_windows.begin()) {
            --child;
            result = child->second;
        }
    }
    if (!result) {
        return std::nullopt;
    }
    const auto start = reinterpret_cast<std::uintptr_t>(result->base);
    return pointer >= start && pointer - start < result->bytes ? result : std::nullopt;
}

AllocationResult MemoryServices::allocate(void *address, std::size_t bytes, std::uint32_t flags,
                                          std::uint32_t protection) noexcept {
    if (!bytes) {
        return {nullptr, error_invalid_parameter};
    }
    if (flags != reserve && flags != commit && flags != (reserve | commit)) {
        return {nullptr, error_not_supported};
    }
    if (address && (flags & reserve)) {
        return {nullptr, error_not_supported};
    }
    if (protection != 1 && protection != 2 && protection != 4 && protection != 0x20) {
        return {nullptr, error_not_supported};
    }
    try {
        const std::lock_guard lock(mutex_);
        const auto page = host::Mapping::page_size();
        std::size_t rounded = 0;
        if (!address) {
            if (!round_up(bytes, page, rounded)) {
                return {nullptr, error_invalid_parameter};
            }
            return reserve_region(rounded, flags, protection, std::nullopt);
        }
        const auto pointer = reinterpret_cast<std::uintptr_t>(address);
        auto found = regions_.upper_bound(pointer);
        if (found == regions_.begin()) {
            return {nullptr, error_invalid_address};
        }
        --found;
        const auto start = pointer - pointer % page;
        const auto offset = start - found->first;
        const auto prefix = pointer - start;
        if (bytes > std::numeric_limits<std::size_t>::max() - prefix ||
            !round_up(bytes + prefix, page, rounded) || offset > found->second->memory.size() ||
            rounded > found->second->memory.size() - offset) {
            return {nullptr, error_invalid_address};
        }
        if (found->second->is_sparse_reservation()) {
            return {nullptr, error_not_supported};
        }
        auto &pages = found->second->protection;
        const auto first = pages.begin() + offset / page;
        const auto last = first + rounded / page;
        if (std::any_of(first, last, [&](auto prior) { return prior && prior != protection; })) {
            return {nullptr, error_not_supported};
        }
        found->second->memory.commit(offset, rounded, access_for(protection));
        std::fill(first, last, protection);
        return {reinterpret_cast<void *>(start), 0};
    } catch (...) {
        return {nullptr, error_not_enough_memory};
    }
}

FreeResult MemoryServices::free(void *address, std::size_t bytes, std::uint32_t flags) noexcept {
    if (flags != release && flags != decommit && flags != (release | preserve_placeholder)) {
        return {false, error_not_supported};
    }
    if (!address || (flags == release && bytes)) {
        return {false, error_invalid_parameter};
    }
    try {
        const std::lock_guard lock(mutex_);
        const auto pointer = reinterpret_cast<std::uintptr_t>(address);
        auto found = regions_.upper_bound(pointer);
        if (found == regions_.begin()) {
            return {false, error_invalid_address};
        }
        --found;
        auto &memory = found->second->memory;
        if (flags == (release | preserve_placeholder)) {
            auto &region = *found->second;
            constexpr std::size_t granularity = 65536;
            if (pointer % granularity || !bytes || bytes % granularity) {
                return {false, error_invalid_parameter};
            }
            if (!region.xmem || region.xmem->backing != XmemBacking::placeholder) {
                return {false, error_not_supported};
            }
            auto segment = region.placeholder_segments.upper_bound(pointer);
            if (segment == region.placeholder_segments.begin()) {
                return {false, error_invalid_address};
            }
            --segment;
            const auto offset = pointer - segment->first;
            if (offset >= segment->second || bytes >= segment->second || bytes > segment->second - offset ||
                (offset && bytes != segment->second - offset) ||
                region.placeholder_windows.contains(segment->first)) {
                return {false, error_invalid_address};
            }
            if (region.placeholder_segments.size() >= 128) {
                return {false, error_not_enough_memory};
            }
            // Prepare both halves before changing ownership metadata. The host
            // mapping and reservation charge stay intact; no physical pages move.
            auto replacement = region.placeholder_segments;
            const auto start = segment->first;
            const auto size = segment->second;
            replacement.erase(start);
            const auto left_bytes = offset ? offset : bytes;
            replacement.emplace(start, left_bytes);
            replacement.emplace(start + left_bytes, size - left_bytes);
            region.placeholder_segments.swap(replacement);
            return {true, 0};
        }
        if (flags == release) {
            if (pointer != found->first) {
                return {false, error_invalid_address};
            }
            if (!found->second->placeholder_windows.empty() ||
                found->second->placeholder_segments.size() > 1) {
                return {false, error_not_supported};
            }
            reserved_ -= memory.size();
            if (found->second->is_sparse_reservation()) {
                sparse_reserved_ -= memory.size();
            }
            regions_.erase(found);
            return {true, 0};
        }
        const auto page = host::Mapping::page_size();
        const auto start = pointer - pointer % page;
        const auto offset = start - found->first;
        std::size_t rounded = 0;
        if (!bytes) {
            if (pointer != found->first) {
                return {false, error_invalid_parameter};
            }
            rounded = memory.size();
        } else if (bytes > std::numeric_limits<std::size_t>::max() - (pointer - start) ||
                   !round_up(bytes + pointer - start, page, rounded)) {
            return {false, error_invalid_address};
        }
        if (offset > memory.size() || rounded > memory.size() - offset) {
            return {false, error_invalid_address};
        }
        if (found->second->is_sparse_reservation()) {
            return {false, error_not_supported};
        }
        memory.decommit(offset, rounded);
        auto &pages = found->second->protection;
        std::fill(pages.begin() + offset / page, pages.begin() + (offset + rounded) / page, 0);
        return {true, 0};
    } catch (...) {
        return {false, error_not_enough_memory};
    }
}

AllocationResult MemoryServices::reserve_window(std::size_t bytes, std::uint64_t attributes,
                                                std::uint32_t protection) noexcept {
    constexpr std::size_t guest_page = 65536;
    std::size_t rounded = 0;
    if (!bytes || !round_up(bytes, guest_page, rounded)) {
        return {nullptr, error_invalid_parameter};
    }
    try {
        const std::lock_guard lock(mutex_);
        return reserve_region(rounded, reserve, 1, XmemRegionInfo{nullptr, rounded, attributes, protection});
    } catch (...) {
        return {nullptr, error_not_enough_memory};
    }
}

AllocationResult MemoryServices::reserve_placeholder(std::size_t bytes, std::uint64_t attributes) noexcept {
    constexpr std::size_t granularity = 65536;
    std::size_t rounded;
    if (!bytes || !round_up(bytes, granularity, rounded)) {
        return {nullptr, error_invalid_parameter};
    }
    try {
        const std::lock_guard lock(mutex_);
        const XmemRegionInfo placeholder{nullptr, rounded,     attributes,
                                         1,       granularity, XmemBacking::placeholder};
        return reserve_region(rounded, reserve, 1, placeholder);
    } catch (...) {
        return {nullptr, error_not_enough_memory};
    }
}

AllocationResult MemoryServices::replace_placeholder_window(void *address, std::size_t bytes,
                                                            std::uint64_t attributes,
                                                            std::uint32_t protection) noexcept {
    constexpr std::size_t granularity = 65536;
    const auto pointer = reinterpret_cast<std::uintptr_t>(address);
    std::size_t rounded;
    if (!address || pointer % granularity || !bytes || !round_up(bytes, granularity, rounded)) {
        return {nullptr, error_invalid_parameter};
    }
    try {
        const std::lock_guard lock(mutex_);
        auto found = regions_.upper_bound(pointer);
        if (found == regions_.begin()) {
            return {nullptr, error_invalid_address};
        }
        --found;
        auto &region = *found->second;
        if (!region.xmem || region.xmem->backing != XmemBacking::placeholder) {
            return {nullptr, error_not_supported};
        }
        const auto offset = pointer - found->first;
        if (offset >= region.memory.size() || rounded > region.memory.size() - offset) {
            return {nullptr, error_invalid_address};
        }
        const auto segment = region.placeholder_segments.find(pointer);
        if (segment == region.placeholder_segments.end() || segment->second != rounded) {
            return {nullptr, error_invalid_address};
        }
        auto next = region.placeholder_windows.lower_bound(pointer);
        if (next != region.placeholder_windows.end() && rounded > next->first - pointer) {
            return {nullptr, error_invalid_address};
        }
        if (next != region.placeholder_windows.begin()) {
            const auto prior = std::prev(next);
            if (pointer - prior->first < prior->second.bytes) {
                return {nullptr, error_invalid_address};
            }
        }
        if (region.placeholder_windows.size() >= 64) {
            return {nullptr, error_not_enough_memory};
        }
        const XmemRegionInfo window{address,    rounded,     attributes,
                                    protection, granularity, XmemBacking::physical_window};
        region.placeholder_windows.emplace(pointer, window);
        return {address, 0};
    } catch (...) {
        return {nullptr, error_not_enough_memory};
    }
}

AllocationResult MemoryServices::reserve_region(std::size_t bytes, std::uint32_t flags,
                                                std::uint32_t protection,
                                                std::optional<XmemRegionInfo> xmem) {
    const bool sparse = xmem && xmem->backing != XmemBacking::private_pages;
    const auto used = sparse ? sparse_reserved_ : reserved_ - sparse_reserved_;
    const auto limit = sparse ? limits_.window_bytes : limits_.private_bytes;
    if (bytes > limit - used || bytes > limits_.total_bytes - reserved_ || regions_.size() >= 1024) {
        return {nullptr, error_not_enough_memory};
    }
    const auto alignment = xmem ? xmem->guest_page_bytes : 65536;
    auto region = std::make_unique<Region>(bytes, alignment);
    auto *base = region->memory.data();
    if (xmem) {
        xmem->base = base;
        region->xmem = xmem;
        if (xmem->backing == XmemBacking::placeholder) {
            region->placeholder_segments.emplace(reinterpret_cast<std::uintptr_t>(base), bytes);
        }
    }
    if (flags & commit) {
        region->memory.commit(0, bytes, access_for(protection));
        std::fill(region->protection.begin(), region->protection.end(), protection);
    }
    regions_.emplace(reinterpret_cast<std::uintptr_t>(base), std::move(region));
    reserved_ += bytes;
    if (sparse) {
        sparse_reserved_ += bytes;
    }
    return {base, 0};
}

AllocationResult MemoryServices::allocate_xmem(std::size_t bytes, std::uint64_t attributes,
                                               std::uint32_t protection,
                                               std::size_t guest_page_bytes) noexcept {
    if (guest_page_bytes != 65536 && guest_page_bytes != 2097152) {
        return {nullptr, error_not_supported};
    }
    if ((protection & 0xff) != 4) {
        return {nullptr, error_not_supported};
    }
    std::size_t rounded = 0;
    if (!bytes || !round_up(bytes, guest_page_bytes, rounded)) {
        return {nullptr, error_invalid_parameter};
    }
    try {
        const std::lock_guard lock(mutex_);
        XmemRegionInfo info{nullptr,    rounded,          attributes,
                            protection, guest_page_bytes, XmemBacking::private_pages};
        return reserve_region(rounded, reserve | commit, 4, info);
    } catch (...) {
        return {nullptr, error_not_enough_memory};
    }
}

std::optional<XmemRegionInfo> MemoryServices::xmem_region(const void *address) const {
    const std::lock_guard lock(mutex_);
    const auto pointer = reinterpret_cast<std::uintptr_t>(address);
    auto found = regions_.upper_bound(pointer);
    if (found == regions_.begin()) {
        return std::nullopt;
    }
    --found;
    const auto &region = *found->second;
    if (pointer - found->first >= region.memory.size()) {
        return std::nullopt;
    }
    auto child = region.placeholder_windows.upper_bound(pointer);
    if (child != region.placeholder_windows.begin()) {
        --child;
        if (pointer - child->first < child->second.bytes) {
            return child->second;
        }
    }
    return region.xmem;
}

std::optional<XmemRegionInfo> MemoryServices::window(const void *address) const {
    const auto region = xmem_region(address);
    return region && region->backing == XmemBacking::physical_window ? region : std::nullopt;
}

std::uint32_t
MemoryServices::map_physical(void *address,
                             std::span<const std::shared_ptr<const PhysicalPage>> pages) noexcept {
    if (pages.empty()) {
        return 0;
    }
    const auto pointer = reinterpret_cast<std::uintptr_t>(address);
    constexpr std::size_t maximum_views = 16384;
    if (!address || pointer % physical_page_bytes || pages.size() > maximum_views) {
        return error_invalid_parameter;
    }
    try {
        const std::lock_guard lock(mutex_);
        auto owner = regions_.upper_bound(pointer);
        if (owner == regions_.begin()) {
            return error_invalid_address;
        }
        --owner;
        auto &region = *owner->second;
        const auto window = region.window_at(pointer);
        if (!window) {
            return error_not_supported;
        }
        const auto window_base = reinterpret_cast<std::uintptr_t>(window->base);
        const auto offset = pointer - window_base;
        const auto bytes = pages.size() * physical_page_bytes;
        if (offset >= window->bytes || bytes > window->bytes - offset) {
            return error_invalid_address;
        }
        host::Access access;
        switch (window->protection & 0xff) {
        case 1:
        case 4:
            // Inherited CPU-renderer projection: GPU-only windows need readable
            // CPU staging. Raw console protection remains in window metadata.
            access = host::Access::read_write;
            break;
        case 2:
            access = host::Access::read;
            break;
        default:
            return error_not_supported;
        }
        const auto first = pointer - owner->first;
        auto replacement = region.physical_views;
        for (std::size_t index = 0; index < pages.size(); ++index) {
            const auto at = first + index * physical_page_bytes;
            if (pages[index]) {
                replacement.insert_or_assign(at, pages[index]);
            } else {
                replacement.erase(at);
            }
        }
        if (replacement.size() > maximum_views) {
            return error_not_enough_memory;
        }
        const auto install = [&](std::size_t at, const std::shared_ptr<const PhysicalPage> &page) {
            if (page) {
                page->map_into(region.memory, at, access);
            } else {
                region.memory.decommit(at, physical_page_bytes);
            }
        };
        std::size_t attempted = 0;
        try {
            for (std::size_t index = 0; index < pages.size(); ++index) {
                ++attempted;
                install(first + index * physical_page_bytes, pages[index]);
            }
        } catch (...) {
            try {
                for (std::size_t index = 0; index < attempted; ++index) {
                    const auto at = first + index * physical_page_bytes;
                    const auto prior = region.physical_views.find(at);
                    install(at, prior == region.physical_views.end() ? nullptr : prior->second);
                }
            } catch (...) {
                // Never release page leases or continue with unknown live aliases.
                std::terminate();
            }
            throw;
        }
        region.physical_views.swap(replacement);
        return 0;
    } catch (...) {
        return error_not_enough_memory;
    }
}

bool MemoryServices::writable_range(const void *address, std::size_t bytes) const noexcept {
    auto cursor = reinterpret_cast<std::uintptr_t>(address);
    if (!address || !bytes || bytes > 64 * 1024 * 1024 || bytes > UINTPTR_MAX - cursor) {
        return false;
    }
    try {
        const std::lock_guard lock(mutex_);
        while (bytes) {
            auto owner = regions_.upper_bound(cursor);
            if (owner == regions_.begin()) {
                return false;
            }
            --owner;
            const auto &region = *owner->second;
            const auto offset = cursor - owner->first;
            if (offset >= region.memory.size()) {
                return false;
            }
            std::size_t step;
            if (region.is_sparse_reservation()) {
                const auto page_offset = offset - offset % physical_page_bytes;
                const auto page = region.physical_views.find(page_offset);
                const auto window = region.window_at(cursor);
                if (!window || page == region.physical_views.end() || !page->second ||
                    ((window->protection & 0xff) != 1 && (window->protection & 0xff) != 4)) {
                    return false;
                }
                step = physical_page_bytes - offset % physical_page_bytes;
            } else {
                const auto page_bytes = host::Mapping::page_size();
                if (region.protection[offset / page_bytes] != 4) {
                    return false;
                }
                step = page_bytes - offset % page_bytes;
            }
            step = std::min({step, bytes, region.memory.size() - offset});
            cursor += step;
            bytes -= step;
        }
        return true;
    } catch (...) {
        return false;
    }
}

PhysicalSpanResult MemoryServices::physical_span(const void *address, std::size_t bytes) const noexcept {
    const auto pointer = reinterpret_cast<std::uintptr_t>(address);
    const auto prefix = pointer % physical_page_bytes;
    constexpr std::size_t maximum_bytes = 16384 * physical_page_bytes;
    if (!address || !bytes || bytes > maximum_bytes - prefix || bytes > UINTPTR_MAX - pointer) {
        return {std::nullopt, error_invalid_parameter};
    }
    try {
        const std::lock_guard lock(mutex_);
        PhysicalSpan span;
        span.bytes_ = bytes;
        span.first_page_offset_ = prefix;
        const auto count = (prefix + bytes - 1) / physical_page_bytes + 1;
        span.pages_.reserve(count);
        const auto first_page = pointer - prefix;
        for (std::size_t index = 0; index < count; ++index) {
            const auto at = first_page + index * physical_page_bytes;
            auto owner = regions_.upper_bound(at);
            if (owner == regions_.begin()) {
                return {std::nullopt, error_invalid_address};
            }
            --owner;
            const auto &region = *owner->second;
            const auto offset = at - owner->first;
            if (offset >= region.memory.size() || physical_page_bytes > region.memory.size() - offset) {
                return {std::nullopt, error_invalid_address};
            }
            const auto window = region.window_at(at);
            const auto page = region.physical_views.find(offset);
            if (!window || page == region.physical_views.end() || !page->second) {
                return {std::nullopt, error_invalid_address};
            }
            span.pages_.push_back({page->second, window->attributes, window->protection});
        }
        return {std::move(span), 0};
    } catch (...) {
        return {std::nullopt, error_not_enough_memory};
    }
}
} // namespace ambrosia::services
