#pragma once
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace ambrosia::host {
// Process-owned scheduler. Jobs are native adapters, never raw guest callbacks.
// Guest entry requires its own runtime scope, TLS and module lifetime guard.
class WorkerPool {
  public:
    static constexpr unsigned thread_count = 2;
    static constexpr unsigned pending_limit = 1024;
    ~WorkerPool();
    void start();
    bool submit(std::function<void()> job);
    // Delayed adapters are cancelled when their owner expires or the pool closes.
    // Immediate work still drains on shutdown; timers never delay shutdown.
    bool submit_after(std::chrono::milliseconds delay, std::weak_ptr<void> owner, std::function<void()> job);
    void shutdown();
    unsigned workers() const;

  private:
    struct DelayedJob {
        std::chrono::steady_clock::time_point due;
        std::weak_ptr<void> owner;
        std::function<void()> callback;
    };

    void run() noexcept;
    void prune_delayed();
    mutable std::mutex mutex_;
    std::mutex lifecycle_mutex_;
    std::condition_variable ready_;
    std::deque<std::function<void()>> pending_;
    std::vector<DelayedJob> delayed_;
    std::vector<std::thread> threads_;
    unsigned workers_ = 0;
    bool stopping_ = false;
    bool closed_ = false;
};
} // namespace ambrosia::host
