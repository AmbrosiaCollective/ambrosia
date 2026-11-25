#include "memory_services.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace ambrosia::services {
namespace {
constexpr std::uint32_t commit = 0x1000;
constexpr std::uint32_t reserve = 0x2000;
constexpr std::uint32_t decommit = 0x4000;
constexpr std::uint32_t release = 0x8000;

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
        if (found->second->window) {
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
    if (flags != release && flags != decommit) {
        return {false, error_not_supported};
    }
    if (!address || (flags == release && bytes)) {
        return {false, error_invalid_parameter};
    }
    try {
        const auto pointer = reinterpret_cast<std::uintptr_t>(address);
        auto found = regions_.upper_bound(pointer);
        if (found == regions_.begin()) {
            return {false, error_invalid_address};
        }
        --found;
        auto &memory = found->second->memory;
        if (flags == release) {
            if (pointer != found->first) {
                return {false, error_invalid_address};
            }
            reserved_ -= memory.size();
            if (found->second->window) {
                window_reserved_ -= memory.size();
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
        if (found->second->window) {
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
        return reserve_region(rounded, reserve, 1, WindowInfo{nullptr, rounded, attributes, protection});
    } catch (...) {
        return {nullptr, error_not_enough_memory};
    }
}

AllocationResult MemoryServices::reserve_region(std::size_t bytes, std::uint32_t flags,
                                                std::uint32_t protection, std::optional<WindowInfo> window) {
    const auto used = window ? window_reserved_ : reserved_ - window_reserved_;
    const auto limit = window ? limits_.window_bytes : limits_.private_bytes;
    if (bytes > limit - used || bytes > limits_.total_bytes - reserved_ || regions_.size() >= 1024) {
        return {nullptr, error_not_enough_memory};
    }
    auto region = std::make_unique<Region>(bytes);
    auto *base = region->memory.data();
    if (window) {
        window->base = base;
        region->window = window;
    } else if (flags & commit) {
        region->memory.commit(0, bytes, access_for(protection));
        std::fill(region->protection.begin(), region->protection.end(), protection);
    }
    regions_.emplace(reinterpret_cast<std::uintptr_t>(base), std::move(region));
    reserved_ += bytes;
    if (window) {
        window_reserved_ += bytes;
    }
    return {base, 0};
}

std::optional<WindowInfo> MemoryServices::window(const void *address) const {
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
    return region.window;
}
} // namespace ambrosia::services
