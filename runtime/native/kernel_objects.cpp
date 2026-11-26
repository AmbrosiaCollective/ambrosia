#include "kernel_objects.h"
#include "event_signal.h"
#include "guest_thread.h"
#include <chrono>
#include <condition_variable>
#include <limits>
#include <stdexcept>

namespace ambrosia::services {
namespace {
struct MutexOwner {};
class MutexObject;
} // namespace

// One cleanup hook per guest thread covers all mutexes in this directory. Weak
// object records avoid extending a closed handle's lifetime or retaining a
// directory after its runtime has gone away.
class MutexOwners : public std::enable_shared_from_this<MutexOwners> {
  public:
    std::shared_ptr<MutexOwner> current();
    void remember(const std::shared_ptr<MutexObject> &object);

  private:
    void abandon(const std::shared_ptr<MutexOwner> &owner);
    std::mutex mutex_;
    std::map<guest::ThreadContext *, std::weak_ptr<MutexOwner>> owners_;
    std::vector<std::weak_ptr<MutexObject>> objects_;
};

namespace {
class MutexObject final : public WaitableObject {
  public:
    explicit MutexObject(std::shared_ptr<MutexOwners> owners) : owners_(std::move(owners)) {}

    WaitResult wait(std::uint32_t milliseconds) override {
        const auto caller = owners_->current();
        std::unique_lock guard(mutex_);
        const auto available = [&] { return !recursion_ || owner_ == caller; };
        if (!available()) {
            if (!milliseconds) {
                return {wait_timeout, 0};
            }
            if (waiters_ == 64) {
                return {wait_failed, 8};
            }
            ++waiters_;
            try {
                if (milliseconds == 0xffffffffu) {
                    changed_.wait(guard, available);
                } else {
                    (void)changed_.wait_for(guard, std::chrono::milliseconds(milliseconds), available);
                }
            } catch (...) {
                --waiters_;
                throw;
            }
            --waiters_;
            if (!available()) {
                return {wait_timeout, 0};
            }
        }
        if (recursion_ == std::numeric_limits<std::int32_t>::max()) {
            throw std::runtime_error("mutex recursive ownership limit exceeded");
        }
        owner_ = caller;
        ++recursion_;
        const auto result = abandoned_ ? wait_abandoned : wait_object_0;
        abandoned_ = false;
        return {result, 0};
    }

    std::uint32_t release() {
        const auto caller = owners_->current();
        {
            const std::lock_guard guard(mutex_);
            if (!recursion_ || owner_ != caller) {
                return 288; // ERROR_NOT_OWNER; no ownership mutation on failure.
            }
            if (--recursion_) {
                return 0;
            }
            owner_.reset();
        }
        changed_.notify_one();
        return 0;
    }

    void abandon(const std::shared_ptr<MutexOwner> &owner) {
        {
            const std::lock_guard guard(mutex_);
            if (!recursion_ || owner_ != owner) {
                return;
            }
            recursion_ = 0;
            owner_.reset();
            abandoned_ = true;
        }
        changed_.notify_all();
    }

  private:
    std::shared_ptr<MutexOwners> owners_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::shared_ptr<MutexOwner> owner_;
    std::int32_t recursion_ = 0;
    std::size_t waiters_ = 0;
    bool abandoned_ = false;
};
} // namespace

std::shared_ptr<MutexOwner> MutexOwners::current() {
    auto *context = guest::ThreadContext::current();
    if (!context) {
        throw std::runtime_error("mutex ownership requires an active guest thread");
    }
    const std::lock_guard guard(mutex_);
    std::erase_if(owners_, [](const auto &entry) { return entry.second.expired(); });
    if (const auto found = owners_.find(context); found != owners_.end()) {
        if (auto owner = found->second.lock()) {
            return owner;
        }
    }
    if (owners_.size() >= 4096) {
        throw std::bad_alloc();
    }
    auto owner = std::make_shared<MutexOwner>();
    context->on_exit([directory = weak_from_this(), owner] {
        if (auto retained = directory.lock()) {
            retained->abandon(owner);
        }
    });
    owners_.insert_or_assign(context, owner);
    return owner;
}

void MutexOwners::remember(const std::shared_ptr<MutexObject> &object) {
    const std::lock_guard guard(mutex_);
    std::erase_if(objects_, [](const auto &entry) { return entry.expired(); });
    if (objects_.size() >= 4096) {
        throw std::bad_alloc();
    }
    objects_.push_back(object);
}

void MutexOwners::abandon(const std::shared_ptr<MutexOwner> &owner) {
    // Never acquire an object's lock while holding the ownership-directory
    // lock. A returning wait/release can be acquiring them in the other order.
    std::vector<std::shared_ptr<MutexObject>> retained;
    {
        const std::lock_guard guard(mutex_);
        for (const auto &entry : objects_) {
            if (auto object = entry.lock()) {
                retained.push_back(std::move(object));
            }
        }
    }
    for (const auto &object : retained) {
        object->abandon(owner);
    }
}

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
    : ids_(std::move(ids)), mutex_owners_(std::make_shared<MutexOwners>()), limit_(limit) {
    if (limit > 4096 || !ids_) {
        throw std::invalid_argument("kernel handle limit exceeds the runtime budget");
    }
}

ObjectResult KernelObjects::insert_waitable(std::shared_ptr<WaitableObject> object, std::uint32_t access) {
    const std::lock_guard lock(mutex_);
    if (!object) {
        return {0, 87};
    }
    if (handles_.size() >= limit_) {
        return {0, 8};
    }
    const auto handle = ids_->allocate();
    if (!handle) {
        return {0, 8};
    }
    handles_.emplace(handle, Handle{std::move(object), access});
    return {handle, 0};
}

std::shared_ptr<WaitableObject> KernelObjects::waitable(std::uintptr_t handle) const {
    const auto entry = find(handle);
    return entry ? entry->object : nullptr;
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

ObjectResult KernelObjects::create_mutex(bool initial_owner, const std::optional<std::u16string> &name) {
    return create_object<MutexObject>(name, 0x1f0001, [&] {
        auto object = std::make_shared<MutexObject>(mutex_owners_);
        mutex_owners_->remember(object);
        // The factory runs only for a new object. Opening an existing name
        // ignores initial_owner and must never acquire it for the caller.
        if (initial_owner) {
            (void)object->wait(0);
        }
        return object;
    });
}

std::uint32_t KernelObjects::release_mutex(std::uintptr_t handle) {
    const auto entry = find(handle);
    const auto object = entry ? std::dynamic_pointer_cast<MutexObject>(entry->object) : nullptr;
    return object ? object->release() : 6;
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
