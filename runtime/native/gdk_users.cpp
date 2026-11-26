#include "gdk_users.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

namespace ambrosia::services {
namespace {
template <unsigned Slot> [[noreturn]] void AMBROSIA_GUEST_ABI unsupported() noexcept {
    diagnostics::stop_trace("unsupported XUserImpl slot " + std::to_string(Slot), 78);
}

struct NativeErrorScope {
    int saved = errno;

    ~NativeErrorScope() {
        errno = saved;
    }
};

Runtime &runtime() {
    auto *owner = current_runtime();
    if (!owner || !owner->gdk_bootstrap().snapshot().references) {
        diagnostics::stop_trace("user subscriptions require an initialized scoped GDK runtime", 78);
    }
    return *owner;
}

std::int32_t AMBROSIA_GUEST_ABI max_users(void *output) noexcept {
    if (!output) {
        return pointer_error;
    }
    // Capacity, not a claim that an account is signed in or authenticated.
    const std::uint32_t capacity = 1;
    std::memcpy(output, &capacity, sizeof(capacity));
    return 0;
}

std::int32_t AMBROSIA_GUEST_ABI register_change(std::uintptr_t queue, void *context,
                                                UserChangeRoutine routine, void *token) noexcept {
    NativeErrorScope restore;
    try {
        auto &owner = runtime();
        return owner.gdk_users().subscribe(owner.task_queues(), queue, context, routine, token);
    } catch (const std::bad_alloc &) {
        return gdk_out_of_memory;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

bool AMBROSIA_GUEST_ABI unregister_change(std::uint64_t token, bool wait) noexcept {
    NativeErrorScope restore;
    try {
        return runtime().gdk_users().unsubscribe(token, wait);
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

std::int32_t AMBROSIA_GUEST_ABI interface_max(void *, void *output) noexcept {
    return max_users(output);
}

std::int32_t AMBROSIA_GUEST_ABI interface_register(void *, std::uintptr_t queue, void *context,
                                                   UserChangeRoutine routine, void *token) noexcept {
    return register_change(queue, context, routine, token);
}

bool AMBROSIA_GUEST_ABI interface_unregister(void *, std::uint64_t token, bool wait) noexcept {
    return unregister_change(token, wait);
}

const std::array<std::uintptr_t, 37> user_vtable{reinterpret_cast<std::uintptr_t>(&interface_query),
                                                 reinterpret_cast<std::uintptr_t>(&interface_add_reference),
                                                 reinterpret_cast<std::uintptr_t>(&interface_release),
                                                 reinterpret_cast<std::uintptr_t>(&unsupported<3>),
                                                 reinterpret_cast<std::uintptr_t>(&unsupported<4>),
                                                 reinterpret_cast<std::uintptr_t>(&unsupported<5>),
                                                 reinterpret_cast<std::uintptr_t>(&interface_max),
                                                 reinterpret_cast<std::uintptr_t>(&unsupported<7>),
                                                 reinterpret_cast<std::uintptr_t>(&unsupported<8>),
                                                 reinterpret_cast<std::uintptr_t>(&unsupported<9>),
                                                 reinterpret_cast<std::uintptr_t>(&unsupported<10>),
                                                 reinterpret_cast<std::uintptr_t>(&unsupported<11>),
                                                 reinterpret_cast<std::uintptr_t>(&unsupported<12>),
                                                 reinterpret_cast<std::uintptr_t>(&unsupported<13>),
                                                 reinterpret_cast<std::uintptr_t>(&unsupported<14>),
                                                 reinterpret_cast<std::uintptr_t>(&unsupported<15>),
                                                 reinterpret_cast<std::uintptr_t>(&unsupported<16>),
                                                 reinterpret_cast<std::uintptr_t>(&unsupported<17>),
                                                 reinterpret_cast<std::uintptr_t>(&unsupported<18>),
                                                 reinterpret_cast<std::uintptr_t>(&unsupported<19>),
                                                 reinterpret_cast<std::uintptr_t>(&unsupported<20>),
                                                 reinterpret_cast<std::uintptr_t>(&unsupported<21>),
                                                 reinterpret_cast<std::uintptr_t>(&unsupported<22>),
                                                 reinterpret_cast<std::uintptr_t>(&unsupported<23>),
                                                 reinterpret_cast<std::uintptr_t>(&unsupported<24>),
                                                 reinterpret_cast<std::uintptr_t>(&unsupported<25>),
                                                 reinterpret_cast<std::uintptr_t>(&unsupported<26>),
                                                 reinterpret_cast<std::uintptr_t>(&unsupported<27>),
                                                 reinterpret_cast<std::uintptr_t>(&unsupported<28>),
                                                 reinterpret_cast<std::uintptr_t>(&unsupported<29>),
                                                 reinterpret_cast<std::uintptr_t>(&unsupported<30>),
                                                 reinterpret_cast<std::uintptr_t>(&unsupported<31>),
                                                 reinterpret_cast<std::uintptr_t>(&unsupported<32>),
                                                 reinterpret_cast<std::uintptr_t>(&interface_register),
                                                 reinterpret_cast<std::uintptr_t>(&interface_unregister),
                                                 reinterpret_cast<std::uintptr_t>(&unsupported<35>),
                                                 reinterpret_cast<std::uintptr_t>(&unsupported<36>)};
} // namespace

GdkUsers::GdkUsers(std::shared_ptr<HandleIds> ids)
    : object_(user_vtable.data(), user_class_id), ids_(std::move(ids)) {
    if (!ids_) {
        throw std::invalid_argument("user subscriptions require a runtime handle directory");
    }
}

std::int32_t GdkUsers::query(const Guid &interface_id, void *output) noexcept {
    return interface_query(&object_, &interface_id, output);
}

std::int32_t GdkUsers::subscribe(const TaskQueues &queues, std::uintptr_t queue, void *context,
                                 UserChangeRoutine routine, void *token) {
    if (!token) {
        return pointer_error;
    }
    const std::uint64_t empty = 0;
    std::memcpy(token, &empty, sizeof(empty));
    if (!routine) {
        return pointer_error;
    }
    if (!queue) {
        diagnostics::stop_trace("user subscription needs the unimplemented process default queue", 78);
    }
    const auto owned_queue = queues.inspect(queue);
    if (!owned_queue) {
        return static_cast<std::int32_t>(0x80070006u);
    }
    std::lock_guard lock(mutex_);
    if (subscriptions_.size() >= registration_limit) {
        return gdk_out_of_memory;
    }
    const auto id = ids_->allocate();
    if (!id) {
        return gdk_out_of_memory;
    }
    subscriptions_.emplace(id, UserSubscription{owned_queue, context, routine});
    const std::uint64_t value = id;
    std::memcpy(token, &value, sizeof(value));
    return 0;
}

bool GdkUsers::unsubscribe(std::uint64_t token, bool wait) {
    // No callbacks can be in flight while the provider has no state changes.
    // Both waiting/nonwaiting removal are therefore complete, not pending.
    (void)wait;
    std::lock_guard lock(mutex_);
    subscriptions_.erase(token);
    return true;
}

std::optional<UserSubscription> GdkUsers::subscription(std::uint64_t token) const {
    std::lock_guard lock(mutex_);
    const auto found = subscriptions_.find(token);
    return found == subscriptions_.end() ? std::nullopt : std::optional{found->second};
}

unsigned GdkUsers::subscriptions() const {
    std::lock_guard lock(mutex_);
    return static_cast<unsigned>(subscriptions_.size());
}

void register_user_services(ImportRegistry &registry) {
    registry.add("xgameruntime.dll", "XUserGetMaxUsers", reinterpret_cast<std::uintptr_t>(&max_users));
    registry.add("xgameruntime.dll", "XUserRegisterForChangeEvent",
                 reinterpret_cast<std::uintptr_t>(&register_change));
    registry.add("xgameruntime.dll", "XUserUnregisterForChangeEvent",
                 reinterpret_cast<std::uintptr_t>(&unregister_change));
}
} // namespace ambrosia::services
