#include "crt_exit.h"
#include "service_runtime.h"
#include "startup_trace.h"

namespace ambrosia::services {
namespace {
std::int32_t AMBROSIA_GUEST_ABI register_exit(Initializer callback) noexcept {
    auto *runtime = current_runtime();
    return runtime ? runtime->exit_callbacks().add(callback) : -1;
}

void AMBROSIA_GUEST_ABI register_tls_exit(TlsExitCallback callback) noexcept {
    auto *runtime = current_runtime();
    if (!runtime) {
        diagnostics::stop_trace("executable TLS cleanup requires a scoped runtime", 78);
    }
    try {
        runtime->exit_callbacks().set_tls(callback);
    } catch (...) {
        diagnostics::stop_trace("executable TLS cleanup registration failed", 78);
    }
}
} // namespace

void register_crt_exit(ImportRegistry &registry) {
    for (const auto *module : {"ucrtbase.dll", "msvcrt.dll", "api-ms-win-crt-runtime-l1-1-0.dll"}) {
        registry.add(module, "_register_thread_local_exe_atexit_callback",
                     reinterpret_cast<std::uintptr_t>(&register_tls_exit));
        registry.add(module, "_crt_atexit", reinterpret_cast<std::uintptr_t>(&register_exit));
        registry.add(module, "atexit", reinterpret_cast<std::uintptr_t>(&register_exit));
    }
}
} // namespace ambrosia::services
