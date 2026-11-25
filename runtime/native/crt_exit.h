#pragma once
#include "crt_lifecycle.h"

namespace ambrosia::services {
using TlsExitCallback = void(AMBROSIA_GUEST_ABI *)(void *, std::uint32_t, void *);

class ExitCallbacks {
  public:
    std::int32_t add(Initializer callback) noexcept;
    void set_tls(TlsExitCallback callback);
    void execute();

  private:
    std::mutex mutex_;
    std::vector<Initializer> callbacks_;
    TlsExitCallback tls_callback_ = nullptr;
    std::size_t executing_ = 0;
    unsigned depth_ = 0;
};

void register_crt_exit(ImportRegistry &registry);
} // namespace ambrosia::services
