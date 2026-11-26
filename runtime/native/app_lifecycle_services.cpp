#include "app_lifecycle.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include <cerrno>
#include <stdexcept>

namespace ambrosia::services {
namespace {
struct RestoreNativeError {
    int saved = errno;

    ~RestoreNativeError() {
        errno = saved;
    }
};

AppLifecycle &lifecycle() {
    auto *runtime = current_runtime();
    if (!runtime) {
        throw std::runtime_error("application notifications require a scoped runtime");
    }
    return runtime->app_lifecycle();
}

template <AppNotification Kind>
std::uint32_t AMBROSIA_GUEST_ABI register_change(AppChangeRoutine routine, void *context,
                                                 void *output) noexcept {
    RestoreNativeError restore;
    try {
        return lifecycle().add(Kind, routine, context, output);
    } catch (const std::bad_alloc &) {
        return 8;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

template <AppNotification Kind>
void AMBROSIA_GUEST_ABI unregister_change(std::uintptr_t registration) noexcept {
    RestoreNativeError restore;
    try {
        lifecycle().remove(Kind, registration);
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}
} // namespace

void register_app_lifecycle(ImportRegistry &registry) {
    for (const auto *module :
         {"api-ms-win-core-psm-appnotify-l1-1-0.dll", "api-ms-win-core-psm-appnotify-l1-1-1.dll",
          "minappmodelx.dll", "kernelx.dll", "xgameruntime.dll"}) {
        registry.add(module, "RegisterAppStateChangeNotification",
                     reinterpret_cast<std::uintptr_t>(&register_change<AppNotification::state>));
        registry.add(module, "RegisterAppConstrainedChangeNotification",
                     reinterpret_cast<std::uintptr_t>(&register_change<AppNotification::constrained>));
        registry.add(module, "UnregisterAppStateChangeNotification",
                     reinterpret_cast<std::uintptr_t>(&unregister_change<AppNotification::state>));
        registry.add(module, "UnregisterAppConstrainedChangeNotification",
                     reinterpret_cast<std::uintptr_t>(&unregister_change<AppNotification::constrained>));
    }
}
} // namespace ambrosia::services
