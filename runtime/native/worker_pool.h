#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
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
    void shutdown();
    unsigned workers() const;

  private:
    void run() noexcept;
    mutable std::mutex mutex_;
    std::mutex lifecycle_mutex_;
    std::condition_variable ready_;
    std::deque<std::function<void()>> pending_;
    std::vector<std::thread> threads_;
    unsigned workers_ = 0;
    bool stopping_ = false;
    bool closed_ = false;
};
} // namespace ambrosia::host
