#include "task_queues.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace ambrosia::services {
namespace {
constexpr auto invalid_handle = static_cast<std::int32_t>(0x80070006u);

void store_handle(void *output, std::uintptr_t value) {
    std::memcpy(output, &value, sizeof(value));
}

bool needs_workers(QueueMode mode) {
    return mode == QueueMode::thread_pool || mode == QueueMode::serialized_thread_pool;
}

TaskQueues &queues() {
    auto *runtime = current_runtime();
    if (!runtime) {
        throw std::runtime_error("task queues require a scoped runtime");
    }
    return runtime->task_queues();
}

struct RestoreNativeError {
    int saved = errno;

    ~RestoreNativeError() {
        errno = saved;
    }
};
} // namespace

std::uintptr_t TaskQueues::publish(std::shared_ptr<QueueState> queue, void *output) {
    const auto handle = ids_->allocate();
    if (!handle) {
        throw std::runtime_error("native task queue handle IDs exhausted");
    }
    handles_.emplace(handle, std::move(queue));
    store_handle(output, handle);
    return handle;
}

std::int32_t TaskQueues::create(std::uint32_t work, std::uint32_t completion, void *output) {
    if (!output) {
        return gdk_invalid_argument;
    }
    store_handle(output, 0);
    if (work > 3 || completion > 3) {
        return gdk_invalid_argument;
    }
    std::lock_guard lock(mutex_);
    if (closing_ || handles_.size() >= handle_limit) {
        return gdk_out_of_memory;
    }
    auto queue =
        std::make_shared<QueueState>(static_cast<QueueMode>(work), static_cast<QueueMode>(completion));
    if (needs_workers(queue->work) || needs_workers(queue->completion)) {
        pool_.start();
    }
    (void)publish(std::move(queue), output);
    return 0;
}

std::int32_t TaskQueues::duplicate(std::uintptr_t handle, void *output) {
    if (!output) {
        return gdk_invalid_argument;
    }
    store_handle(output, 0);
    std::lock_guard lock(mutex_);
    const auto found = handles_.find(handle);
    if (found == handles_.end()) {
        return invalid_handle;
    }
    if (closing_ || handles_.size() >= handle_limit) {
        return gdk_out_of_memory;
    }
    (void)publish(found->second, output);
    return 0;
}

void TaskQueues::close(std::uintptr_t handle) {
    std::lock_guard lock(mutex_);
    handles_.erase(handle);
}

std::shared_ptr<const QueueState> TaskQueues::inspect(std::uintptr_t handle) const {
    std::lock_guard lock(mutex_);
    const auto found = handles_.find(handle);
    return found == handles_.end() ? nullptr : found->second;
}

unsigned TaskQueues::workers() const {
    return pool_.workers();
}

void TaskQueues::shutdown() {
    {
        const std::lock_guard lock(mutex_);
        closing_ = true;
    }
    // Never hold the directory lock while joining a callback that may use it.
    pool_.shutdown();
}

NativeQueueScheduler TaskQueues::thread_pool_scheduler(std::uintptr_t handle, QueuePort port) {
    const auto queue = inspect(handle);
    if (!queue || (port != QueuePort::work && port != QueuePort::completion)) {
        return {};
    }
    const auto mode = port == QueuePort::work ? queue->work : queue->completion;
    if (mode != QueueMode::thread_pool) {
        return {};
    }
    // The closure retains the actual queue after its public handle closes. It
    // belongs to this runtime; all users must stop before pool destruction ends.
    return [this, queue, port](std::uint32_t delay, std::weak_ptr<void> owner, std::function<void()> job) {
        const auto mode = port == QueuePort::work ? queue->work : queue->completion;
        if (mode != QueueMode::thread_pool) {
            return false;
        }
        return pool_.submit_after(std::chrono::milliseconds(delay), std::move(owner), std::move(job));
    };
}

std::int32_t AMBROSIA_GUEST_ABI task_queue_create(std::uint32_t work, std::uint32_t completion,
                                                  void *output) noexcept {
    RestoreNativeError restore;
    try {
        return queues().create(work, completion, output);
    } catch (const std::bad_alloc &) {
        return gdk_out_of_memory;
    } catch (const std::system_error &) {
        return gdk_out_of_memory;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

std::int32_t AMBROSIA_GUEST_ABI task_queue_duplicate(std::uintptr_t handle, void *output) noexcept {
    RestoreNativeError restore;
    try {
        return queues().duplicate(handle, output);
    } catch (const std::bad_alloc &) {
        return gdk_out_of_memory;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

void AMBROSIA_GUEST_ABI task_queue_close(std::uintptr_t handle) noexcept {
    RestoreNativeError restore;
    try {
        queues().close(handle);
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

void register_task_queues(ImportRegistry &registry) {
    registry.add("xgameruntime.dll", "XTaskQueueCreate",
                 reinterpret_cast<std::uintptr_t>(&task_queue_create));
    registry.add("xgameruntime.dll", "XTaskQueueDuplicateHandle",
                 reinterpret_cast<std::uintptr_t>(&task_queue_duplicate));
    registry.add("xgameruntime.dll", "XTaskQueueCloseHandle",
                 reinterpret_cast<std::uintptr_t>(&task_queue_close));
}
} // namespace ambrosia::services
