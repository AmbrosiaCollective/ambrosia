#pragma once
#include "handle_ids.h"
#include "semaphore_signal.h"
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace ambrosia::services {
inline constexpr std::uint32_t wait_object_0 = 0;
inline constexpr std::uint32_t wait_timeout = 258;
inline constexpr std::uint32_t wait_failed = 0xffffffffu;
inline constexpr std::uint32_t object_synchronize = 0x100000;
inline constexpr std::uint32_t object_modify_state = 2;
inline constexpr std::uint32_t object_all_access = 0x1f0003;

struct WaitResult {
    std::uint32_t result = wait_failed;
    std::uint32_t error = 0;
};

struct ObjectResult {
    std::uintptr_t handle = 0;
    std::uint32_t error = 0;
};

// Future thread/timer objects can share the same owned handle directory and
// wait boundary. Guest handles are opaque IDs and are never host pointers/FDs.
class WaitableObject {
  public:
    virtual ~WaitableObject() = default;
    virtual WaitResult wait(std::uint32_t milliseconds) = 0;
};

class KernelObjects {
  public:
    explicit KernelObjects(std::size_t limit = 4096,
                           std::shared_ptr<HandleIds> ids = std::make_shared<HandleIds>());
    ObjectResult create_event(bool manual_reset, bool initial_state,
                              const std::optional<std::u16string> &name = std::nullopt);
    ObjectResult create_semaphore(std::int32_t initial, std::int32_t maximum,
                                  const std::optional<std::u16string> &name = std::nullopt,
                                  std::uint32_t access = object_all_access);
    SemaphoreRelease release_semaphore(std::uintptr_t handle, std::int32_t amount);
    std::uint32_t set_event(std::uintptr_t handle);
    std::uint32_t reset_event(std::uintptr_t handle);
    WaitResult wait(std::uintptr_t handle, std::uint32_t milliseconds);
    std::uint32_t close(std::uintptr_t handle);

  private:
    struct Handle {
        std::shared_ptr<WaitableObject> object;
        std::uint32_t access;
    };
    template <class Object, class Factory>
    ObjectResult create_object(const std::optional<std::u16string> &name, std::uint32_t access,
                               Factory factory);
    std::optional<Handle> find(std::uintptr_t handle) const;
    mutable std::mutex mutex_;
    std::map<std::uintptr_t, Handle> handles_;
    std::map<std::u16string, std::weak_ptr<WaitableObject>> names_;
    std::shared_ptr<HandleIds> ids_;
    std::size_t limit_;
    std::size_t name_bytes_ = 0;
};
} // namespace ambrosia::services
