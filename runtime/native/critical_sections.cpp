#include "critical_sections.h"
#include "host.h"
#include "memory_services.h"
#include <cstring>
#include <exception>
#include <limits>
#include <stdexcept>

namespace ambrosia::services {
namespace {
// Guest layout follows the x64 RTL_CRITICAL_SECTION ABI. It stays opaque to
// clients; all native mutex objects live outside guest memory.
struct GuestSection {
    std::uint64_t debug = 0;
    std::int32_t lock_count = -1;
    std::int32_t recursion = 0;
    std::uint64_t owner = 0;
    std::uint64_t semaphore = 0;
    std::uint64_t spin = 0;
};

static_assert(sizeof(GuestSection) == 40);
static_assert(offsetof(GuestSection, owner) == 16);
static_assert(offsetof(GuestSection, spin) == 32);
} // namespace

struct CriticalSections::State {
    std::recursive_mutex native;
    std::mutex metadata;
    GuestSection guest;
    bool deleted = false;

    void publish(void *address) const {
        std::memcpy(address, &guest, sizeof(guest));
    }
};

CriticalSections::~CriticalSections() {
    for (const auto &[address, state] : sections_) {
        (void)address;
        if (state->guest.recursion) {
            // Destroying an owned native mutex is undefined. Report an invalid
            // runtime teardown rather than silently destroying a held lock.
            std::terminate();
        }
    }
}

std::uint32_t CriticalSections::initialize(void *address, std::uint32_t spin, std::uint32_t flags) noexcept {
    if (!address || reinterpret_cast<std::uintptr_t>(address) % 8) {
        return error_invalid_parameter;
    }
    if (flags & ~0x01000000u) {
        return error_not_supported;
    }
    try {
        const std::lock_guard guard(directory_);
        if (sections_.contains(address)) {
            return error_invalid_parameter;
        }
        if (sections_.size() >= 4096) {
            return error_not_enough_memory;
        }
        auto state = std::make_shared<State>();
        state->guest.spin = spin;
        // Spin count is an optimization hint. Blocking/recursive ownership is
        // provided by the native mutex; no fake kernel handle/debugger state.
        state->publish(address);
        sections_.emplace(address, std::move(state));
        return 0;
    } catch (...) {
        return error_not_enough_memory;
    }
}

std::shared_ptr<CriticalSections::State> CriticalSections::find(void *address) const {
    const std::lock_guard guard(directory_);
    const auto found = sections_.find(address);
    if (found == sections_.end()) {
        throw std::runtime_error("critical section is not initialized in this runtime");
    }
    return found->second;
}

bool CriticalSections::enter(void *address, bool try_only) {
    const auto state = find(address);
    if (try_only) {
        if (!state->native.try_lock()) {
            return false;
        }
    } else {
        state->native.lock();
    }
    try {
        const std::lock_guard guard(state->metadata);
        if (state->deleted || state->guest.recursion == std::numeric_limits<std::int32_t>::max()) {
            throw std::runtime_error("critical section was deleted or recursion overflowed");
        }
        const auto thread = host::current_thread_id();
        if (state->guest.recursion && state->guest.owner != thread) {
            throw std::runtime_error("critical section has inconsistent native ownership");
        }
        state->guest.owner = thread;
        ++state->guest.recursion;
        state->guest.lock_count = state->guest.recursion - 1;
        state->publish(address);
    } catch (...) {
        state->native.unlock();
        throw;
    }
    return true;
}

void CriticalSections::leave(void *address) {
    const auto state = find(address);
    {
        const std::lock_guard guard(state->metadata);
        if (!state->guest.recursion || state->guest.owner != host::current_thread_id()) {
            throw std::runtime_error("critical section leave requires calling-thread ownership");
        }
        --state->guest.recursion;
        state->guest.lock_count = state->guest.recursion - 1;
        if (!state->guest.recursion) {
            state->guest.owner = 0;
        }
        state->publish(address);
    }
    state->native.unlock();
}

void CriticalSections::remove(void *address) {
    const std::lock_guard directory(directory_);
    const auto found = sections_.find(address);
    if (found == sections_.end()) {
        throw std::runtime_error("critical section deletion requires an initialized object");
    }
    const auto state = found->second;
    const std::lock_guard guard(state->metadata);
    if (state->guest.recursion) {
        throw std::runtime_error("cannot delete an owned critical section");
    }
    state->deleted = true;
    state->guest = {};
    state->publish(address);
    sections_.erase(found);
}

std::uint32_t CriticalSections::set_spin(void *address, std::uint32_t spin) {
    const auto state = find(address);
    const std::lock_guard guard(state->metadata);
    if (state->deleted) {
        throw std::runtime_error("cannot change a deleted critical section");
    }
    const auto previous = static_cast<std::uint32_t>(state->guest.spin);
    state->guest.spin = spin;
    state->publish(address);
    return previous;
}

std::size_t CriticalSections::size() const {
    const std::lock_guard guard(directory_);
    return sections_.size();
}
} // namespace ambrosia::services
