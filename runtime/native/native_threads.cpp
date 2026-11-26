#include "native_threads.h"
#include "host_priority.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <stdexcept>

namespace ambrosia::services {
namespace {
std::uint32_t priority_error(host::SchedulingError error) {
    switch (error) {
    case host::SchedulingError::none:
        return 0;
    case host::SchedulingError::denied:
        return 5;
    case host::SchedulingError::missing_thread:
        return 6;
    case host::SchedulingError::invalid:
        return 87;
    case host::SchedulingError::unsupported:
        return 50;
    case host::SchedulingError::failure:
        return 31;
    }
    return 31;
}

class ThreadCompletion final : public WaitableObject {
  public:
    WaitResult wait(std::uint32_t milliseconds) override {
        std::unique_lock lock(mutex);
        if (done) {
            return {wait_object_0, 0};
        }
        if (!milliseconds) {
            return {wait_timeout, 0};
        }
        if (waiters == 64) {
            return {wait_failed, 8};
        }
        ++waiters;
        try {
            if (milliseconds == 0xffffffffu) {
                changed.wait(lock, [&] { return done; });
            } else {
                (void)changed.wait_for(lock, std::chrono::milliseconds(milliseconds), [&] { return done; });
            }
        } catch (...) {
            --waiters;
            throw;
        }
        --waiters;
        return {done ? wait_object_0 : wait_timeout, 0};
    }

    void ready(std::uint32_t value, std::uint32_t error) {
        std::lock_guard lock(mutex);
        id = value;
        start_error = error;
        prepared = true;
        changed.notify_all();
    }

    void finish(std::uint32_t value) {
        std::lock_guard lock(mutex);
        code = value;
        done = true;
        changed.notify_all();
    }

    std::mutex mutex;
    std::condition_variable changed;
    bool prepared = false;
    bool launched = false;
    std::uint32_t suspend_count = 0;
    std::int32_t priority = 0;
    int base_niceness = 0;
    bool done = false;
    unsigned waiters = 0;
    std::uint32_t id = 0;
    std::uint32_t start_error = 0;
    std::uint32_t code = 259; // STILL_ACTIVE until all guest cleanup completes.
};
} // namespace

struct NativeThreads::Job {
    std::shared_ptr<ThreadCompletion> completion = std::make_shared<ThreadCompletion>();
    std::thread thread;
    std::atomic<bool> finished{false};
};

NativeThreads::~NativeThreads() {
    shutdown();
}

void NativeThreads::reap_finished() {
    // Called with the directory lock. Finished is published only after all
    // guest callbacks/cleanup, so joining cannot wait for recursive creation.
    std::erase_if(jobs_, [](const auto &job) {
        if (!job->finished.load(std::memory_order_acquire)) {
            return false;
        }
        job->thread.join();
        return true;
    });
}

ThreadCreation NativeThreads::create(Runtime &runtime, pe::ModuleGraph &graph, ThreadEntry entry,
                                     void *argument, std::size_t stack_bytes, bool suspended) {
    if (!entry) {
        return {0, 0, 87};
    }
    if (stack_bytes > stack_limit) {
        return {0, 0, 8};
    }
    const auto snapshot = graph.thread_snapshot();
    std::unique_lock lock(mutex_);
    if (closing_) {
        return {0, 0, 995};
    }
    reap_finished();
    if (jobs_.size() >= 64) {
        return {0, 0, 8};
    }
    auto job = std::make_shared<Job>();
    job->completion->suspend_count = suspended ? 1 : 0;
    jobs_.push_back(job);
    ObjectResult object;
    try {
        object = objects_.insert_waitable(job->completion, 0x1fffff);
        if (object.error) {
            jobs_.pop_back();
            return {0, 0, object.error};
        }
        job->thread = std::thread([job, snapshot, &runtime, &graph, entry, argument, stack_bytes] {
            try {
                host::reset_worker_guest_segment();
                diagnostics::ThreadFaultStack fault_stack;
                const auto bounds = host::current_stack_bounds();
                if (bounds.upper <= bounds.lower || bounds.upper - bounds.lower < stack_bytes) {
                    job->completion->ready(0, 8);
                } else {
                    const auto id = host::current_thread_id();
                    const auto scheduling = host::thread_niceness(id);
                    if (scheduling.error != host::SchedulingError::none) {
                        job->completion->ready(0, priority_error(scheduling.error));
                        job->finished.store(true, std::memory_order_release);
                        return;
                    }
                    job->completion->base_niceness = scheduling.value;
                    job->completion->ready(id, 0);
                    {
                        std::unique_lock gate(job->completion->mutex);
                        job->completion->changed.wait(gate, [&] {
                            return job->completion->launched && !job->completion->suspend_count;
                        });
                    }
                    std::uint32_t code;
                    {
                        Scope scope(runtime, &graph);
                        code = snapshot.run([&] {
                            diagnostics::write_service_trace("TRACE native guest thread entry\n");
                            return entry(argument);
                        });
                    }
                    job->completion->finish(code);
                }
            } catch (const std::exception &error) {
                bool prepared;
                {
                    std::lock_guard state(job->completion->mutex);
                    prepared = job->completion->prepared;
                }
                if (prepared) {
                    diagnostics::stop_trace(error.what(), 78);
                }
                job->completion->ready(0, 8);
            }
            job->finished.store(true, std::memory_order_release);
        });
    } catch (...) {
        if (object.handle) {
            (void)objects_.close(object.handle);
        }
        jobs_.pop_back();
        throw;
    }
    try {
        std::unique_lock state(job->completion->mutex);
        job->completion->changed.wait(state, [&] { return job->completion->prepared; });
        const auto id = job->completion->id;
        const auto error = job->completion->start_error;
        if (error) {
            state.unlock();
            job->thread.join();
            (void)objects_.close(object.handle);
            jobs_.pop_back();
            return {0, 0, error};
        }
        // Publishing the handle and native ID precedes guest entry. A child may
        // start another thread immediately, so release the directory before entry.
        lock.unlock();
        job->completion->launched = true;
        state.unlock();
        job->completion->changed.notify_all();
        return {object.handle, id, 0};
    } catch (const std::exception &) {
        // After a host worker exists, an unexpected synchronization failure
        // cannot be reported as a clean creation failure with a stranded job.
        diagnostics::stop_trace("native thread startup handshake failed", 78);
    }
}

std::uint32_t NativeThreads::exit_code(std::uintptr_t handle, void *output) const {
    if (!output) {
        return 87;
    }
    const auto state = std::dynamic_pointer_cast<ThreadCompletion>(objects_.waitable(handle));
    if (!state) {
        return 6;
    }
    const std::lock_guard lock(state->mutex);
    std::memcpy(output, &state->code, sizeof(state->code));
    return 0;
}

std::uint32_t NativeThreads::thread_id(std::uintptr_t handle) const {
    const auto state = std::dynamic_pointer_cast<ThreadCompletion>(objects_.waitable(handle));
    if (!state) {
        return 0;
    }
    const std::lock_guard lock(state->mutex);
    return state->id;
}

std::uint32_t NativeThreads::set_priority(std::uintptr_t handle, std::int32_t priority) {
    const auto state = std::dynamic_pointer_cast<ThreadCompletion>(objects_.waitable(handle));
    if (!state) {
        return 6;
    }
    int adjustment;
    switch (priority) {
    case -15:
        adjustment = 19;
        break;
    case -2:
        adjustment = 10;
        break;
    case -1:
        adjustment = 5;
        break;
    case 0:
        adjustment = 0;
        break;
    case 1:
        adjustment = -5;
        break;
    case 2:
        adjustment = -10;
        break;
    case 15:
        adjustment = -20;
        break;
    default:
        return 87;
    }
    // Completion cannot publish/exit/recycle its native TID while this lock
    // protects the scheduling syscall. Only live owned IDs reach the host.
    std::lock_guard lock(state->mutex);
    if (state->done) {
        return 6;
    }
    const auto target = std::clamp(state->base_niceness + adjustment, -20, 19);
    const auto error = priority_error(host::set_thread_niceness(state->id, target));
    if (!error) {
        state->priority = priority;
    }
    return error;
}

ThreadPriority NativeThreads::priority(std::uintptr_t handle) const {
    const auto state = std::dynamic_pointer_cast<ThreadCompletion>(objects_.waitable(handle));
    if (!state) {
        return {0x7fffffff, 6};
    }
    const std::lock_guard lock(state->mutex);
    return {state->priority, 0};
}

ThreadResume NativeThreads::resume(std::uintptr_t handle) {
    const auto state = std::dynamic_pointer_cast<ThreadCompletion>(objects_.waitable(handle));
    if (!state) {
        return {0xffffffffu, 6};
    }
    std::lock_guard lock(state->mutex);
    const auto previous = state->suspend_count;
    if (previous) {
        --state->suspend_count;
        state->changed.notify_all();
    }
    return {previous, 0};
}

void NativeThreads::shutdown() noexcept {
    try {
        if (std::this_thread::get_id() != owner_) {
            diagnostics::stop_trace("native thread directory must shut down on its owning host thread", 78);
        }
        std::vector<std::shared_ptr<Job>> jobs;
        {
            std::lock_guard lock(mutex_);
            closing_ = true;
            jobs.swap(jobs_);
        }
        for (const auto &job : jobs) {
            {
                const std::lock_guard state(job->completion->mutex);
                if (job->completion->suspend_count && !job->completion->done) {
                    diagnostics::stop_trace(
                        "shutdown of an unresumed guest thread requires unmodeled thread termination", 78);
                }
            }
            job->thread.join();
        }
    } catch (...) {
        diagnostics::stop_trace("cannot join native guest workers before process detach", 78);
    }
}
} // namespace ambrosia::services
