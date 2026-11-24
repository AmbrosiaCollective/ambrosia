#include "guest_thread.h"
#include <algorithm>
#include <stdexcept>

namespace ambrosia::guest {
namespace {
thread_local ThreadContext *active_context = nullptr;
}

ThreadContext::ThreadContext() : owner_(std::this_thread::get_id()) {
    anchor_.tls = slots_.data();
}

void ThreadContext::require_owner() const {
    if (owner_ != std::this_thread::get_id()) {
        throw std::runtime_error("guest context used on another host thread");
    }
}

void ThreadContext::install(std::size_t slot, const TlsTemplate &spec) {
    require_owner();
    if (sealed_ || slot >= slots_.size() || blocks_[slot]) {
        throw std::runtime_error("guest TLS slot is unavailable");
    }
    constexpr std::size_t limit = 1024 * 1024;
    if (!spec.alignment || spec.alignment > 8192 || (spec.alignment & (spec.alignment - 1)) ||
        spec.initial.size() > limit || spec.zero_fill > limit - spec.initial.size()) {
        throw std::runtime_error("invalid guest TLS template");
    }
    auto block = std::make_unique<Block>();
    block->size = spec.initial.size() + spec.zero_fill;
    block->storage.resize(std::max<std::size_t>(1, block->size) + spec.alignment - 1);
    const auto address = reinterpret_cast<std::uintptr_t>(block->storage.data());
    block->offset = (spec.alignment - address % spec.alignment) % spec.alignment;
    std::copy(spec.initial.begin(), spec.initial.end(), block->storage.begin() + block->offset);
    slots_[slot] = block->storage.data() + block->offset;
    blocks_[slot] = std::move(block);
}

ThreadContext::Activation ThreadContext::activate() {
    require_owner();
    sealed_ = true;
    return Activation(*this);
}

ThreadContext *ThreadContext::current() noexcept {
    return active_context;
}

std::uint32_t &ThreadContext::last_error() {
    require_owner();
    return anchor_.last_error;
}

ThreadContext::Activation::Activation(ThreadContext &context)
    : segment_(&context.anchor_), saved_(active_context) {
    active_context = &context;
}

ThreadContext::Activation::~Activation() {
    active_context = saved_;
}

std::span<std::byte> ThreadContext::block(std::size_t slot) {
    require_owner();
    if (slot >= blocks_.size() || !blocks_[slot]) {
        throw std::runtime_error("guest TLS slot is absent");
    }
    auto &block = *blocks_[slot];
    return {block.storage.data() + block.offset, block.size};
}
} // namespace ambrosia::guest
