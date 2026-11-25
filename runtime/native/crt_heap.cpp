#include "crt_heap.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace ambrosia::services {
CrtHeap::~CrtHeap() {
    // Runtime destruction must follow guest/context cleanup. Outstanding guest
    // allocations belong to this runtime and never become process-global leaks.
    for (const auto &[address, bytes] : allocations_) {
        (void)bytes;
        std::free(address);
    }
}

HeapResult CrtHeap::allocate(std::size_t bytes, bool zero) noexcept {
    bytes = std::max<std::size_t>(bytes, 1);
    try {
        const std::lock_guard guard(mutex_);
        if (bytes > budget_ - used_ || allocations_.size() >= 65536) {
            return {nullptr, HeapError::exhausted};
        }
        void *address = std::malloc(bytes);
        if (!address) {
            return {nullptr, HeapError::exhausted};
        }
        try {
            allocations_.emplace(address, bytes);
        } catch (...) {
            std::free(address);
            return {nullptr, HeapError::exhausted};
        }
        if (zero) {
            std::memset(address, 0, bytes);
        }
        used_ += bytes;
        return {address, HeapError::none};
    } catch (...) {
        return {nullptr, HeapError::exhausted};
    }
}

HeapResult CrtHeap::resize(void *address, std::size_t bytes) noexcept {
    if (!address) {
        return allocate(bytes);
    }
    if (!bytes) {
        return {nullptr, release(address) ? HeapError::none : HeapError::foreign_pointer};
    }
    try {
        const std::lock_guard guard(mutex_);
        const auto found = allocations_.find(address);
        if (found == allocations_.end()) {
            return {nullptr, HeapError::foreign_pointer};
        }
        const auto previous = found->second;
        if (bytes > budget_ - (used_ - previous)) {
            return {nullptr, HeapError::exhausted};
        }
        // Retain the existing map node: successful realloc can move storage,
        // and updating its key must not allocate or throw after that move.
        auto node = allocations_.extract(found);
        void *resized = std::realloc(address, bytes);
        if (!resized) {
            allocations_.insert(std::move(node));
            return {nullptr, HeapError::exhausted};
        }
        node.key() = resized;
        node.mapped() = bytes;
        allocations_.insert(std::move(node));
        used_ = used_ - previous + bytes;
        return {resized, HeapError::none};
    } catch (...) {
        return {nullptr, HeapError::exhausted};
    }
}

bool CrtHeap::release(void *address) noexcept {
    if (!address) {
        return true;
    }
    try {
        const std::lock_guard guard(mutex_);
        const auto found = allocations_.find(address);
        if (found == allocations_.end()) {
            return false;
        }
        used_ -= found->second;
        allocations_.erase(found);
        std::free(address);
        return true;
    } catch (...) {
        return false;
    }
}
} // namespace ambrosia::services
