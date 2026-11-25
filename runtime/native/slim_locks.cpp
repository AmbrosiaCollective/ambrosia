#include "slim_locks.h"
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace ambrosia::services {
struct SlimLocks::State {
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t readers = 0;
    std::size_t waiting_writers = 0;
    std::size_t waiters = 0;
    bool exclusive = false;
};

std::shared_ptr<SlimLocks::State> SlimLocks::find(void *address, bool initializing) {
    if (!address || reinterpret_cast<std::uintptr_t>(address) % 8) {
        throw std::runtime_error("SRW lock address is null or misaligned");
    }
    const std::lock_guard guard(directory_);
    if (const auto found = locks_.find(address); found != locks_.end()) {
        return found->second;
    }
    if (locks_.size() >= 4096) {
        throw std::bad_alloc();
    }
    if (!initializing) {
        std::uint64_t value;
        std::memcpy(&value, address, sizeof(value));
        if (value) {
            throw std::runtime_error("unowned nonzero SRW lock state is unsupported");
        }
    }
    auto state = std::make_shared<State>();
    locks_.emplace(address, state);
    return state;
}

void SlimLocks::initialize(void *address) {
    const auto state = find(address, true);
    const std::lock_guard guard(state->mutex);
    if (state->exclusive || state->readers || state->waiters) {
        throw std::runtime_error("SRW lock cannot be initialized while in use");
    }
    const std::uint64_t zero = 0;
    std::memcpy(address, &zero, sizeof(zero));
}

bool SlimLocks::acquire(void *address, bool shared, bool try_only) {
    const auto state = find(address);
    std::unique_lock guard(state->mutex);
    const auto available = [&] {
        return !state->exclusive && (shared ? state->waiting_writers == 0 : state->readers == 0);
    };
    if (!available()) {
        if (try_only) {
            return false;
        }
        if (state->waiters == 64) {
            throw std::bad_alloc();
        }
        ++state->waiters;
        if (!shared) {
            ++state->waiting_writers;
        }
        const auto leave_queue = [&] {
            --state->waiters;
            if (!shared) {
                --state->waiting_writers;
            }
        };
        try {
            state->changed.wait(guard, available);
        } catch (...) {
            leave_queue();
            state->changed.notify_all();
            throw;
        }
        leave_queue();
    }
    if (shared) {
        if (state->readers == 4096) {
            throw std::bad_alloc();
        }
        ++state->readers;
    } else {
        state->exclusive = true;
    }
    return true;
}

void SlimLocks::release(void *address, bool shared) {
    const auto state = find(address);
    {
        const std::lock_guard guard(state->mutex);
        if (shared) {
            if (!state->readers || state->exclusive) {
                throw std::runtime_error("SRW shared release requires a shared acquisition");
            }
            --state->readers;
        } else {
            if (!state->exclusive) {
                throw std::runtime_error("SRW exclusive release requires an exclusive acquisition");
            }
            state->exclusive = false;
        }
    }
    // Predicate checks admit readers together or one writer, with waiting
    // writers ahead of new readers. No FIFO fairness guarantee is imposed.
    state->changed.notify_all();
}
} // namespace ambrosia::services
