#include "crt_exit.h"
#include <stdexcept>

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

void ExitCallbacks::execute() {
    std::vector<Initializer> pending;
    {
        const std::lock_guard guard(mutex_);
        if (depth_ >= 32) {
            throw std::runtime_error("CRT cleanup recursion exceeds the runtime budget");
        }
        pending.swap(callbacks_);
        executing_ += pending.size();
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
