#include "semaphore_signal.h"
#include <algorithm>
#include <stdexcept>

namespace ambrosia::services {
SemaphoreSignal::SemaphoreSignal(std::int32_t initial, std::int32_t maximum)
    : count_(initial), maximum_(maximum) {
    if (initial < 0 || maximum <= 0 || initial > maximum) {
        throw std::invalid_argument("invalid semaphore initial/maximum count");
    }
    waiters_.reserve(64);
}

bool SemaphoreSignal::poll() {
    if (!count_) {
        return false;
    }
    --count_;
    return true;
}

SemaphoreSignal::ArmResult SemaphoreSignal::arm(Waiter &waiter) {
    if (waiter.released || std::find(waiters_.begin(), waiters_.end(), &waiter) != waiters_.end()) {
        throw std::runtime_error("semaphore waiter is already armed or released");
    }
    if (poll()) {
        waiter.released = true;
        return ArmResult::ready;
    }
    if (waiters_.size() >= 64) {
        return ArmResult::exhausted;
    }
    waiters_.push_back(&waiter);
    return ArmResult::queued;
}

void SemaphoreSignal::cancel(Waiter &waiter) {
    std::erase(waiters_, &waiter);
}

SemaphoreRelease SemaphoreSignal::release(std::int32_t amount) {
    if (amount <= 0) {
        return {87, count_};
    }
    // Validate before handoff, even if pending waiters would consume permits.
    // Subtraction avoids signed overflow at the largest legal LONG maximum.
    if (amount > maximum_ - count_) {
        return {298, count_}; // ERROR_TOO_MANY_POSTS.
    }
    const auto previous = count_;
    auto remaining = amount;
    while (remaining && !waiters_.empty()) {
        waiters_.front()->released = true;
        waiters_.erase(waiters_.begin());
        --remaining;
    }
    count_ += remaining;
    return {0, previous};
}
} // namespace ambrosia::services
