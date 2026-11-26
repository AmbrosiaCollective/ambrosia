#pragma once
#include "handle_ids.h"
#include "runtime_interface.h"
#include <map>
#include <mutex>
#include <optional>

namespace ambrosia::services {
inline constexpr Guid game_input_interface_id{
    0x11be2a7e, 0x4254, 0x445a, {0x9c, 0x09, 0xff, 0xc4, 0x0f, 0x00, 0x69, 0x18}};
inline constexpr std::int32_t input_device_not_found = static_cast<std::int32_t>(0x838a0002u);
inline constexpr std::int32_t input_reading_not_found = static_cast<std::int32_t>(0x838a0003u);
enum class InputCallbackKind { reading, device, guide, keyboard_layout };

struct InputSubscription {
    InputCallbackKind kind;
    std::uint32_t input_kinds = 0;
    std::uint32_t status_filter = 0;
    std::int32_t enumeration = 0;
    float analog_threshold = 0;
    void *context = nullptr;
    std::uintptr_t routine = 0;
    bool enabled = true;
};

// The CPU-only profile has no device backend. This models its real empty
// collection; readings, host input and callback delivery need a separate backend.
class GameInput {
  public:
    static constexpr unsigned callback_limit = 128;
    explicit GameInput(std::shared_ptr<HandleIds> ids);
    std::int32_t create(void *output) noexcept;
    std::int32_t add(InputSubscription entry, void *token);
    void stop(std::uint64_t token);
    bool remove(std::uint64_t token, std::uint64_t timeout_us);
    std::optional<InputSubscription> subscription(std::uint64_t token) const;
    unsigned subscriptions() const;

    std::uint32_t references() const noexcept {
        return object_.references.load();
    }

  private:
    RuntimeInterface object_;
    std::shared_ptr<HandleIds> ids_;
    mutable std::mutex mutex_;
    std::map<std::uint64_t, InputSubscription> subscriptions_;
};

void register_game_input(ImportRegistry &registry);
} // namespace ambrosia::services
