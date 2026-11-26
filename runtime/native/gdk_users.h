#pragma once
#include "handle_ids.h"
#include "runtime_interface.h"
#include "task_queues.h"
#include <map>
#include <mutex>
#include <optional>

namespace ambrosia::services {
inline constexpr Guid user_class_id{
    0x01acd177, 0x91f9, 0x4763, {0xa3, 0x8e, 0xcc, 0xbb, 0x55, 0xce, 0x32, 0xe0}};
using UserChangeRoutine = void(AMBROSIA_GUEST_ABI *)(void *context, std::uint64_t local_id,
                                                     std::uint32_t event);

struct UserSubscription {
    std::shared_ptr<const QueueState> queue;
    void *context;
    UserChangeRoutine routine;
};

// No signed-in user or state transitions exist yet. Registration owns its real
// callback/context/queue without inventing an initial event. Event dispatch and
// user creation require the guest asynchronous queue implementation.
class GdkUsers {
  public:
    static constexpr unsigned registration_limit = 128;
    explicit GdkUsers(std::shared_ptr<HandleIds> ids);
    std::int32_t query(const Guid &interface_id, void *output) noexcept;
    std::int32_t subscribe(const TaskQueues &queues, std::uintptr_t queue, void *context,
                           UserChangeRoutine routine, void *token);
    bool unsubscribe(std::uint64_t token, bool wait);
    std::optional<UserSubscription> subscription(std::uint64_t token) const;
    unsigned subscriptions() const;

    std::uint32_t references() const noexcept {
        return object_.references.load();
    }

  private:
    RuntimeInterface object_;
    std::shared_ptr<HandleIds> ids_;
    mutable std::mutex mutex_;
    std::map<std::uint64_t, UserSubscription> subscriptions_;
};

void register_user_services(ImportRegistry &registry);
} // namespace ambrosia::services
