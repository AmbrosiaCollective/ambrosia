#include "crt_exit.h"
#include <stdexcept>
#include <utility>

namespace ambrosia::services {
std::int32_t ExitCallbacks::add(Initializer callback) noexcept {
    if (!callback) {
        return -1;
    }
    try {
        const std::lock_guard guard(mutex_);
        if (callbacks_.size() + executing_ >= 4096) {
            return -1;
        }
        callbacks_.push_back(callback);
        return 0;
    } catch (...) {
        return -1;
    }
}

void ExitCallbacks::set_tls(TlsExitCallback callback) {
    const std::lock_guard guard(mutex_);
    tls_callback_ = callback;
}

void ExitCallbacks::execute() {
    std::vector<Initializer> pending;
    TlsExitCallback tls = nullptr;
    {
        const std::lock_guard guard(mutex_);
        if (depth_ >= 32) {
            throw std::runtime_error("CRT cleanup recursion exceeds the runtime budget");
        }
        tls = std::exchange(tls_callback_, nullptr);
        ++depth_;
    }
    auto finish = [&] {
        const std::lock_guard guard(mutex_);
        executing_ -= pending.size();
        --depth_;
    };
    // Do not hold the registry lock across guest code. A callback can register
    // a fresh batch, which stays owned for a later explicit cleanup execution.
    try {
        if (tls) {
            // This executable's CRT TLS destructors precede ordinary process
            // callbacks and module detach. Do not confuse this with the PE's
            // loader TLS directory or a worker-thread detach notification.
            tls(nullptr, 0, nullptr);
        }
        {
            const std::lock_guard guard(mutex_);
            // A TLS destructor may register an ordinary process callback. It
            // belongs to this cleanup, so snapshot the list after TLS runs.
            pending.swap(callbacks_);
            executing_ += pending.size();
        }
        for (auto callback = pending.rbegin(); callback != pending.rend(); ++callback) {
            (*callback)();
        }
    } catch (...) {
        finish();
        throw;
    }
    finish();
}
} // namespace ambrosia::services
