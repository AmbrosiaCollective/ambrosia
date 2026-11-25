#include "worker_pool.h"
#include "host.h"
#include "startup_trace.h"
#include <stdexcept>

namespace ambrosia::host {
WorkerPool::~WorkerPool() {
    shutdown();
}

void WorkerPool::start() {
    std::lock_guard lifecycle(lifecycle_mutex_);
    std::unique_lock lock(mutex_);
    if (closed_) {
        throw std::runtime_error("native worker pool is closed");
    }
    if (!threads_.empty()) {
        return;
    }
    try {
        threads_.reserve(thread_count);
        for (unsigned index = 0; index < thread_count; ++index) {
            threads_.emplace_back([this] { run(); });
        }
        workers_ = thread_count;
    } catch (...) {
        stopping_ = true;
        lock.unlock();
        ready_.notify_all();
        for (auto &thread : threads_) {
            thread.join();
        }
        lock.lock();
        threads_.clear();
        stopping_ = false;
        throw;
    }
}

bool WorkerPool::submit(std::function<void()> job) {
    std::lock_guard lock(mutex_);
    if (!job || !workers_ || stopping_ || closed_ || pending_.size() >= pending_limit) {
        return false;
    }
    pending_.push_back(std::move(job));
    ready_.notify_one();
    return true;
}

void WorkerPool::shutdown() {
    std::lock_guard lifecycle(lifecycle_mutex_);
    {
        std::lock_guard lock(mutex_);
        stopping_ = closed_ = true;
    }
    ready_.notify_all();
    for (auto &thread : threads_) {
        // A scheduler owner must outlive its jobs and shut down off-worker.
        if (thread.get_id() == std::this_thread::get_id()) {
            diagnostics::stop_trace("native worker pool cannot join its own callback", 78);
        }
        thread.join();
    }
    std::lock_guard lock(mutex_);
    threads_.clear();
    workers_ = 0;
}

unsigned WorkerPool::workers() const {
    std::lock_guard lock(mutex_);
    return workers_;
}

void WorkerPool::run() noexcept {
    try {
        reset_worker_guest_segment();
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
            if (pending_.empty()) {
                return;
            }
            job = std::move(pending_.front());
            pending_.pop_front();
        }
        try {
            job();
        } catch (...) {
            diagnostics::stop_trace("exception escaped a native worker adapter", 78);
        }
    }
}
} // namespace ambrosia::host
