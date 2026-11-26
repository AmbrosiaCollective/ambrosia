#pragma once
#include "gdk_bootstrap.h"
#include "handle_ids.h"
#include "worker_pool.h"
#include <map>
#include <memory>

namespace ambrosia::services {
enum class QueueMode : std::uint32_t { manual, thread_pool, serialized_thread_pool, immediate };

struct QueueState {
    QueueMode work;
    QueueMode completion;
};

using NativeQueueScheduler = std::function<bool(std::uint32_t, std::weak_ptr<void>, std::function<void()>)>;

// Handles are runtime-owned opaque IDs, not host pointers or kernel handles.
// Dispatch/submission/termination remain separate capabilities.
class TaskQueues {
  public:
    static constexpr unsigned handle_limit = 128;

    explicit TaskQueues(std::shared_ptr<HandleIds> ids) : ids_(std::move(ids)) {}

    std::int32_t create(std::uint32_t work, std::uint32_t completion, void *output);
    std::int32_t duplicate(std::uintptr_t handle, void *output);
    void close(std::uintptr_t handle);
    std::shared_ptr<const QueueState> inspect(std::uintptr_t handle) const;
    unsigned workers() const;
    void shutdown();
    // Native adapters only. Other work modes need their own dispatch contract.
    NativeQueueScheduler thread_pool_scheduler(std::uintptr_t handle);

  private:
    std::uintptr_t publish(std::shared_ptr<QueueState> queue, void *output);
    std::shared_ptr<HandleIds> ids_;
    mutable std::mutex mutex_;
    std::map<std::uintptr_t, std::shared_ptr<QueueState>> handles_;
    bool closing_ = false;
    host::WorkerPool pool_;
};

std::int32_t AMBROSIA_GUEST_ABI task_queue_create(std::uint32_t work, std::uint32_t completion,
                                                  void *output) noexcept;
std::int32_t AMBROSIA_GUEST_ABI task_queue_duplicate(std::uintptr_t handle, void *output) noexcept;
void AMBROSIA_GUEST_ABI task_queue_close(std::uintptr_t handle) noexcept;
void register_task_queues(ImportRegistry &registry);
} // namespace ambrosia::services
