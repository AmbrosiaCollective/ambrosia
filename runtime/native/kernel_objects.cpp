#include "kernel_objects.h"
#include "event_signal.h"
#include <chrono>
#include <condition_variable>
#include <limits>
#include <stdexcept>

namespace ambrosia::services {
namespace {
class EventObject final : public WaitableObject {
  public:
    EventObject(bool manual_reset, bool initial_state) : state_(manual_reset, initial_state) {}

    WaitResult wait(std::uint32_t milliseconds) override {
        std::unique_lock guard(mutex_);
        if (state_.poll()) {
            return {wait_object_0, 0};
        }
        if (!milliseconds) {
            return {wait_timeout, 0};
        }
        EventSignal::Waiter waiter;
        if (state_.arm(waiter) == EventSignal::ArmResult::exhausted) {
            return {wait_failed, 8};
        }
        try {
            if (milliseconds == 0xffffffffu) {
                changed_.wait(guard, [&] { return waiter.released; });
            } else {
                (void)changed_.wait_for(guard, std::chrono::milliseconds(milliseconds),
                                        [&] { return waiter.released; });
            }
        } catch (...) {
            state_.cancel(waiter);
            throw;
        }
        state_.cancel(waiter);
        return {waiter.released ? wait_object_0 : wait_timeout, 0};
    }

    void signal() {
        {
            const std::lock_guard guard(mutex_);
            state_.signal();
        }
        // Only assigned waiters satisfy their predicates; notify_all ensures
        // the selected auto-reset waiter cannot remain asleep behind another.
        changed_.notify_all();
    }

    void reset() {
        const std::lock_guard guard(mutex_);
        state_.reset();
    }

  private:
    std::mutex mutex_;
    std::condition_variable changed_;
    EventSignal state_;
};
} // namespace

KernelObjects::KernelObjects(std::size_t limit) : limit_(limit) {
    if (limit > 4096) {
        throw std::invalid_argument("kernel handle limit exceeds the runtime budget");
    }
}

ObjectResult KernelObjects::create_event(bool manual_reset, bool initial_state,
                                         const std::optional<std::u16string> &name) {
    if (name && (name->empty() || name->size() > 32767 || name->find(char16_t{}) != std::u16string::npos)) {
        return {0, 123};
    }
    const std::lock_guard guard(mutex_);
    if (handles_.size() >= limit_ || next_handle_ > std::numeric_limits<std::uintptr_t>::max() - 4) {
        return {0, 8};
    }
    std::shared_ptr<WaitableObject> object;
    std::uint32_t error = 0;
    if (name) {
        std::erase_if(names_, [&](const auto &entry) {
            if (!entry.second.expired()) {
                return false;
            }
            name_bytes_ -= entry.first.size() * sizeof(char16_t);
            return true;
        });
        if (const auto found = names_.find(*name); found != names_.end()) {
            object = found->second.lock();
        }
        if (object) {
            error = 183; // ERROR_ALREADY_EXISTS; original reset/state settings remain.
        }
    }
    if (!object) {
        if (name &&
            (names_.size() >= limit_ || name->size() * sizeof(char16_t) > 1024 * 1024 - name_bytes_)) {
            return {0, 8};
        }
        object = std::make_shared<EventObject>(manual_reset, initial_state);
    }
    const auto handle = next_handle_;
    handles_.emplace(handle, object);
    try {
        if (name) {
            names_.insert_or_assign(*name, object);
            if (!error) {
                name_bytes_ += name->size() * sizeof(char16_t);
            }
        }
    } catch (...) {
        handles_.erase(handle);
        throw;
    }
    next_handle_ += 4;
    return {handle, error};
}

std::shared_ptr<WaitableObject> KernelObjects::find(std::uintptr_t handle) const {
    const std::lock_guard guard(mutex_);
    const auto found = handles_.find(handle);
    return found == handles_.end() ? nullptr : found->second;
}

std::uint32_t KernelObjects::set_event(std::uintptr_t handle) {
    const auto object = std::dynamic_pointer_cast<EventObject>(find(handle));
    if (!object) {
        return 6;
    }
    object->signal();
    return 0;
}

std::uint32_t KernelObjects::reset_event(std::uintptr_t handle) {
    const auto object = std::dynamic_pointer_cast<EventObject>(find(handle));
    if (!object) {
        return 6;
    }
    object->reset();
    return 0;
}

WaitResult KernelObjects::wait(std::uintptr_t handle, std::uint32_t milliseconds) {
    // Retaining a reference keeps the object alive if another handle closes
    // while this wait is in flight; the directory lock never spans blocking.
    const auto object = find(handle);
    return object ? object->wait(milliseconds) : WaitResult{wait_failed, 6};
}

std::uint32_t KernelObjects::close(std::uintptr_t handle) {
    const std::lock_guard guard(mutex_);
    return handles_.erase(handle) ? 0 : 6;
}
} // namespace ambrosia::services
