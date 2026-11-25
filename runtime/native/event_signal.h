#pragma once
#include <vector>

namespace ambrosia::services {
// Portable signal/handoff model. Its owner supplies synchronization. A queued
// waiter must stay alive until released or explicitly cancelled.
class EventSignal {
  public:
    struct Waiter {
        bool released = false;
    };
    enum class ArmResult { ready, queued, exhausted };
    EventSignal(bool manual_reset, bool initially_signaled);
    bool poll();
    ArmResult arm(Waiter &waiter);
    void cancel(Waiter &waiter);
    void signal();
    void reset();

  private:
    bool manual_reset_;
    bool signaled_;
    std::vector<Waiter *> waiters_;
};
} // namespace ambrosia::services
