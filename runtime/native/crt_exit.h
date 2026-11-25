#pragma once
#include "crt_lifecycle.h"

namespace ambrosia::services {
class ExitCallbacks {
  public:
    std::int32_t add(Initializer callback) noexcept;
    void execute();

  private:
    std::mutex mutex_;
    std::vector<Initializer> callbacks_;
    std::size_t executing_ = 0;
    unsigned depth_ = 0;
};

void register_crt_exit(ImportRegistry &registry);
} // namespace ambrosia::services
