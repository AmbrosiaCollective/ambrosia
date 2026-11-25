#pragma once
#include <cstdint>
#include <vector>

namespace ambrosia::services {
struct SemaphoreRelease {
    std::uint32_t error = 0;
    std::int32_t previous = 0;
};

// Caller-synchronized permit model. A released waiter owns its assigned permit;
// a new poll cannot steal it before the waiter resumes on its host thread.
class SemaphoreSignal {
  public:
    struct Waiter {
        bool released = false;
    };
    enum class ArmResult { ready, queued, exhausted };
    SemaphoreSignal(std::int32_t initial, std::int32_t maximum);
    bool poll();
    ArmResult arm(Waiter &waiter);
    void cancel(Waiter &waiter);
    SemaphoreRelease release(std::int32_t amount);

  private:
    std::int32_t count_;
    std::int32_t maximum_;
    std::vector<Waiter *> waiters_;
};
} // namespace ambrosia::services
