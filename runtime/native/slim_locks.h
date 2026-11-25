#pragma once
#include "import_registry.h"
#include <map>
#include <memory>
#include <mutex>

namespace ambrosia::services {
// SRWLOCK is an opaque pointer-sized guest value. Native mutex/condition state
// stays owned here; zero-initialized guest locks are registered lazily.
class SlimLocks {
  public:
    void initialize(void *address);
    bool acquire(void *address, bool shared, bool try_only);
    void release(void *address, bool shared);

  private:
    struct State;
    std::shared_ptr<State> find(void *address, bool initializing = false);
    std::mutex directory_;
    std::map<void *, std::shared_ptr<State>> locks_;
};

void register_slim_lock_services(ImportRegistry &registry);
} // namespace ambrosia::services
