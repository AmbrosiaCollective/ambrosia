#include "kernel_objects.h"
#include "event_signal.h"
#include <chrono>
#include <condition_variable>
#include <stdexcept>

namespace ambrosia::services {
namespace {
template <class Signal> class SignalObject : public WaitableObject {
  public:
    template <class... Arguments> explicit SignalObject(Arguments... arguments) : state_(arguments...) {}

    WaitResult wait(std::uint32_t milliseconds) override {
        std::unique_lock guard(mutex_);
        if (state_.poll()) {
            return {wait_object_0, 0};
        }
        if (!milliseconds) {
            return {wait_timeout, 0};
        }
        typename Signal::Waiter waiter;
        if (state_.arm(waiter) == Signal::ArmResult::exhausted) {
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

  protected:
    std::mutex mutex_;
    std::condition_variable changed_;
    Signal state_;
};

class EventObject final : public SignalObject<EventSignal> {
  public:
    EventObject(bool manual_reset, bool initial_state) : SignalObject(manual_reset, initial_state) {}

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
};

class SemaphoreObject final : public SignalObject<SemaphoreSignal> {
  public:
    SemaphoreObject(std::int32_t initial, std::int32_t maximum) : SignalObject(initial, maximum) {}

    SemaphoreRelease release(std::int32_t amount) {
        SemaphoreRelease result;
        {
            const std::lock_guard guard(mutex_);
            result = state_.release(amount);
        }
        if (!result.error) {
            changed_.notify_all();
        }
        return result;
    }
};
} // namespace

KernelObjects::KernelObjects(std::size_t limit, std::shared_ptr<HandleIds> ids)
    : ids_(std::move(ids)), limit_(limit) {
    if (limit > 4096 || !ids_) {
        throw std::invalid_argument("kernel handle limit exceeds the runtime budget");
    }
}

template <class Object, class Factory>
ObjectResult KernelObjects::create_object(const std::optional<std::u16string> &name, std::uint32_t access,
                                          Factory factory) {
    if (name && (name->empty() || name->size() > 32767 || name->find(char16_t{}) != std::u16string::npos)) {
        return {0, 123};
    }
    const std::lock_guard guard(mutex_);
    if (handles_.size() >= limit_) {
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
            if (!std::dynamic_pointer_cast<Object>(object)) {
                return {0, 6}; // Named kernel objects share one typed namespace.
            }
            error = 183; // Existing object retains the original settings/state.
        }
    }
    if (!object) {
        if (name &&
            (names_.size() >= limit_ || name->size() * sizeof(char16_t) > 1024 * 1024 - name_bytes_)) {
            return {0, 8};
        }
        object = factory();
    }
    const auto handle = ids_->allocate();
    if (!handle) {
        return {0, 8};
    }
    handles_.emplace(handle, Handle{object, access});
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
    return {handle, error};
}

ObjectResult KernelObjects::create_event(bool manual_reset, bool initial_state,
                                         const std::optional<std::u16string> &name) {
    return create_object<EventObject>(
        name, object_all_access, [&] { return std::make_shared<EventObject>(manual_reset, initial_state); });
}

ObjectResult KernelObjects::create_semaphore(std::int32_t initial, std::int32_t maximum,
                                             const std::optional<std::u16string> &name,
                                             std::uint32_t access) {
    if (initial < 0 || maximum <= 0 || initial > maximum) {
        return {0, 87};
    }
    if (access & ~object_all_access) {
        return {0, 5};
    }
    return create_object<SemaphoreObject>(
        name, access, [&] { return std::make_shared<SemaphoreObject>(initial, maximum); });
}

std::optional<KernelObjects::Handle> KernelObjects::find(std::uintptr_t handle) const {
    const std::lock_guard guard(mutex_);
    const auto found = handles_.find(handle);
    return found == handles_.end() ? std::nullopt : std::optional(found->second);
}

std::uint32_t KernelObjects::set_event(std::uintptr_t handle) {
    const auto entry = find(handle);
    const auto object = entry ? std::dynamic_pointer_cast<EventObject>(entry->object) : nullptr;
    if (!object) {
        return 6;
    }
    if (!(entry->access & object_modify_state)) {
        return 5;
    }
    object->signal();
    return 0;
}

std::uint32_t KernelObjects::reset_event(std::uintptr_t handle) {
    const auto entry = find(handle);
    const auto object = entry ? std::dynamic_pointer_cast<EventObject>(entry->object) : nullptr;
    if (!object) {
        return 6;
    }
    if (!(entry->access & object_modify_state)) {
        return 5;
    }
    object->reset();
    return 0;
}

WaitResult KernelObjects::wait(std::uintptr_t handle, std::uint32_t milliseconds) {
    // Retaining a reference keeps the object alive if another handle closes
    // while this wait is in flight; the directory lock never spans blocking.
    const auto entry = find(handle);
    if (!entry) {
        return {wait_failed, 6};
    }
    if (!(entry->access & object_synchronize)) {
        return {wait_failed, 5};
    }
    return entry->object->wait(milliseconds);
}

SemaphoreRelease KernelObjects::release_semaphore(std::uintptr_t handle, std::int32_t amount) {
    const auto entry = find(handle);
    const auto object = entry ? std::dynamic_pointer_cast<SemaphoreObject>(entry->object) : nullptr;
    if (!object) {
        return {6, 0};
    }
    if (!(entry->access & object_modify_state)) {
        return {5, 0};
    }
    return object->release(amount);
}

std::uint32_t KernelObjects::close(std::uintptr_t handle) {
    const std::lock_guard guard(mutex_);
    return handles_.erase(handle) ? 0 : 6;
}
} // namespace ambrosia::services
