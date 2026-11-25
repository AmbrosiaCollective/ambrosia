#include "event_signal.h"
#include <algorithm>
#include <stdexcept>

namespace ambrosia::services {
EventSignal::EventSignal(bool manual_reset, bool initially_signaled)
    : manual_reset_(manual_reset), signaled_(initially_signaled) {
    waiters_.reserve(64);
}

bool EventSignal::poll() {
    if (!signaled_) {
        return false;
    }
    if (!manual_reset_) {
        signaled_ = false;
    }
    return true;
}

EventSignal::ArmResult EventSignal::arm(Waiter &waiter) {
    if (waiter.released || std::find(waiters_.begin(), waiters_.end(), &waiter) != waiters_.end()) {
        throw std::runtime_error("event waiter is already armed or released");
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

void EventSignal::cancel(Waiter &waiter) {
    std::erase(waiters_, &waiter);
}

void EventSignal::signal() {
    if (manual_reset_) {
        signaled_ = true;
        for (auto *waiter : waiters_) {
            waiter->released = true;
        }
        waiters_.clear();
    } else if (!waiters_.empty()) {
        // Handoff is committed now. A later poll/reset cannot steal the signal
        // before this selected waiter resumes on its host thread.
        waiters_.front()->released = true;
        waiters_.erase(waiters_.begin());
    } else {
        signaled_ = true;
    }
}

void EventSignal::reset() {
    signaled_ = false;
}
} // namespace ambrosia::services
