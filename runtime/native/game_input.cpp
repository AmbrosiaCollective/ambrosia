#include "game_input.h"
#include "gdk_bootstrap.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include <cerrno>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

namespace ambrosia::services {
namespace {
template <unsigned Slot> [[noreturn]] void AMBROSIA_GUEST_ABI unsupported() noexcept {
    diagnostics::stop_trace("unsupported IGameInput slot " + std::to_string(Slot), 78);
}

struct NativeErrorScope {
    int saved = errno;

    ~NativeErrorScope() {
        errno = saved;
    }
};

GameInput &input() {
    auto *owner = current_runtime();
    if (!owner) {
        diagnostics::stop_trace("GameInput requires a scoped native runtime", 78);
    }
    return owner->game_input();
}

std::int32_t AMBROSIA_GUEST_ABI create(void *output) noexcept {
    NativeErrorScope restore;
    return input().create(output);
}

std::uint64_t AMBROSIA_GUEST_ABI timestamp(void *) noexcept {
    NativeErrorScope restore;
    const auto ticks = host::performance_counter();
    if (ticks < 0) {
        diagnostics::stop_trace("input timestamp requires a nonnegative native counter", 78);
    }
    return static_cast<std::uint64_t>(ticks) / 1000;
}

std::int32_t AMBROSIA_GUEST_ABI current_reading(void *, std::uint32_t kind, void *device,
                                                void *output) noexcept {
    NativeErrorScope restore;
    (void)kind;
    if (!output) {
        return pointer_error;
    }
    void *empty = nullptr;
    std::memcpy(output, &empty, sizeof(empty));
    // A nonnull pointer cannot identify a device in this empty collection.
    return device ? input_device_not_found : input_reading_not_found;
}

std::int32_t subscribe(void *device, InputSubscription entry, void *token) noexcept {
    NativeErrorScope restore;
    if (!token) {
        return pointer_error;
    }
    const std::uint64_t empty = 0;
    std::memcpy(token, &empty, sizeof(empty));
    if (!entry.routine) {
        return pointer_error;
    }
    if (device) {
        return input_device_not_found;
    }
    if (entry.enumeration < 0 || entry.enumeration > 2 || !std::isfinite(entry.analog_threshold) ||
        entry.analog_threshold < 0) {
        return gdk_invalid_argument;
    }
    try {
        return input().add(entry, token);
    } catch (const std::bad_alloc &) {
        return gdk_out_of_memory;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

using ReadingRoutine = void(AMBROSIA_GUEST_ABI *)(std::uint64_t, void *, void *, bool);
using DeviceRoutine = void(AMBROSIA_GUEST_ABI *)(std::uint64_t, void *, void *, std::uint64_t, std::uint32_t,
                                                 std::uint32_t);
using GuideRoutine = void(AMBROSIA_GUEST_ABI *)(std::uint64_t, void *, void *, std::uint64_t, bool);
using LayoutRoutine = void(AMBROSIA_GUEST_ABI *)(std::uint64_t, void *, void *, std::uint64_t, std::uint32_t,
                                                 std::uint32_t);

std::int32_t AMBROSIA_GUEST_ABI register_reading(void *, void *device, std::uint32_t kinds, float threshold,
                                                 void *context, ReadingRoutine routine,
                                                 void *token) noexcept {
    return subscribe(device,
                     {InputCallbackKind::reading, kinds, 0, 0, threshold, context,
                      reinterpret_cast<std::uintptr_t>(routine), true},
                     token);
}

std::int32_t AMBROSIA_GUEST_ABI register_device(void *, void *device, std::uint32_t kinds,
                                                std::uint32_t filter, std::int32_t enumeration, void *context,
                                                DeviceRoutine routine, void *token) noexcept {
    return subscribe(device,
                     {InputCallbackKind::device, kinds, filter, enumeration, 0, context,
                      reinterpret_cast<std::uintptr_t>(routine), true},
                     token);
}

std::int32_t AMBROSIA_GUEST_ABI register_guide(void *, void *device, void *context, GuideRoutine routine,
                                               void *token) noexcept {
    return subscribe(
        device,
        {InputCallbackKind::guide, 0, 0, 0, 0, context, reinterpret_cast<std::uintptr_t>(routine), true},
        token);
}

std::int32_t AMBROSIA_GUEST_ABI register_layout(void *, void *device, void *context, LayoutRoutine routine,
                                                void *token) noexcept {
    return subscribe(device,
                     {InputCallbackKind::keyboard_layout, 0, 0, 0, 0, context,
                      reinterpret_cast<std::uintptr_t>(routine), true},
                     token);
}

void AMBROSIA_GUEST_ABI stop_callback(void *, std::uint64_t token) noexcept {
    NativeErrorScope restore;
    try {
        input().stop(token);
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

bool AMBROSIA_GUEST_ABI unregister_callback(void *, std::uint64_t token, std::uint64_t timeout) noexcept {
    NativeErrorScope restore;
    try {
        return input().remove(token, timeout);
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

void AMBROSIA_GUEST_ABI set_focus(void *, std::uint32_t policy) noexcept {
    if (policy) {
        diagnostics::stop_trace("nondefault GameInput focus policy requires a native window input backend",
                                78);
    }
    // No windows/devices exist in this CPU-only provider, so default focus has
    // no OS state to change. A backend must implement policies before attaching.
}

const std::array<std::uintptr_t, 22> input_vtable{reinterpret_cast<std::uintptr_t>(&interface_query),
                                                  reinterpret_cast<std::uintptr_t>(&interface_add_reference),
                                                  reinterpret_cast<std::uintptr_t>(&interface_release),
                                                  reinterpret_cast<std::uintptr_t>(&timestamp),
                                                  reinterpret_cast<std::uintptr_t>(&current_reading),
                                                  reinterpret_cast<std::uintptr_t>(&unsupported<5>),
                                                  reinterpret_cast<std::uintptr_t>(&unsupported<6>),
                                                  reinterpret_cast<std::uintptr_t>(&unsupported<7>),
                                                  reinterpret_cast<std::uintptr_t>(&register_reading),
                                                  reinterpret_cast<std::uintptr_t>(&register_device),
                                                  reinterpret_cast<std::uintptr_t>(&register_guide),
                                                  reinterpret_cast<std::uintptr_t>(&register_layout),
                                                  reinterpret_cast<std::uintptr_t>(&stop_callback),
                                                  reinterpret_cast<std::uintptr_t>(&unregister_callback),
                                                  reinterpret_cast<std::uintptr_t>(&unsupported<14>),
                                                  reinterpret_cast<std::uintptr_t>(&unsupported<15>),
                                                  reinterpret_cast<std::uintptr_t>(&unsupported<16>),
                                                  reinterpret_cast<std::uintptr_t>(&unsupported<17>),
                                                  reinterpret_cast<std::uintptr_t>(&unsupported<18>),
                                                  reinterpret_cast<std::uintptr_t>(&unsupported<19>),
                                                  reinterpret_cast<std::uintptr_t>(&unsupported<20>),
                                                  reinterpret_cast<std::uintptr_t>(&set_focus)};
} // namespace

GameInput::GameInput(std::shared_ptr<HandleIds> ids)
    : object_(input_vtable.data(), game_input_interface_id), ids_(std::move(ids)) {
    if (!ids_) {
        throw std::invalid_argument("input callbacks require a runtime handle directory");
    }
}

std::int32_t GameInput::create(void *output) noexcept {
    return interface_query(&object_, &game_input_interface_id, output);
}

std::int32_t GameInput::add(InputSubscription entry, void *token) {
    std::lock_guard lock(mutex_);
    if (subscriptions_.size() >= callback_limit) {
        return gdk_out_of_memory;
    }
    const auto id = ids_->allocate();
    if (!id) {
        return gdk_out_of_memory;
    }
    subscriptions_.emplace(id, entry);
    const std::uint64_t value = id;
    std::memcpy(token, &value, sizeof(value));
    return 0;
}

void GameInput::stop(std::uint64_t token) {
    std::lock_guard lock(mutex_);
    const auto found = subscriptions_.find(token);
    if (found != subscriptions_.end()) {
        found->second.enabled = false;
    }
}

bool GameInput::remove(std::uint64_t token, std::uint64_t timeout_us) {
    (void)timeout_us;
    // This empty backend cannot have an in-flight device callback to wait for.
    std::lock_guard lock(mutex_);
    subscriptions_.erase(token);
    return true;
}

std::optional<InputSubscription> GameInput::subscription(std::uint64_t token) const {
    std::lock_guard lock(mutex_);
    const auto found = subscriptions_.find(token);
    return found == subscriptions_.end() ? std::nullopt : std::optional{found->second};
}

unsigned GameInput::subscriptions() const {
    std::lock_guard lock(mutex_);
    return static_cast<unsigned>(subscriptions_.size());
}

void register_game_input(ImportRegistry &registry) {
    registry.add("gameinput.dll", "GameInputCreate", reinterpret_cast<std::uintptr_t>(&create));
    registry.add_ordinal("gameinput.dll", 1, "GameInputCreate");
    registry.add("xgameruntime.dll", "GameInputCreate", reinterpret_cast<std::uintptr_t>(&create));
}
} // namespace ambrosia::services
