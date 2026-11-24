#include "loader_services.h"
#include "module_graph.h"
#include "service_runtime.h"

namespace ambrosia::services {
namespace {
std::int32_t AMBROSIA_GUEST_ABI disable_thread_calls(void *module) noexcept {
    try {
        auto *modules = current_modules();
        if (modules && modules->disable_thread_notifications(reinterpret_cast<std::uintptr_t>(module))) {
            return 1;
        }
    } catch (...) {
        // The module graph enforces its host-thread owner and valid lifetime.
    }
    set_last_error(126); // ERROR_MOD_NOT_FOUND, including DLLs with active static TLS.
    return 0;
}
} // namespace

void register_loader_services(ImportRegistry &registry) {
    for (const auto *module :
         {"kernel32.dll", "kernelbase.dll", "api-ms-win-core-libraryloader-l1-1-0.dll",
          "api-ms-win-core-libraryloader-l1-1-1.dll", "api-ms-win-core-libraryloader-l1-2-0.dll",
          "api-ms-win-core-libraryloader-l1-2-1.dll", "api-ms-win-core-libraryloader-l1-2-2.dll",
          "api-ms-win-core-libraryloader-l1-2-3.dll"}) {
        registry.add(module, "DisableThreadLibraryCalls",
                     reinterpret_cast<std::uintptr_t>(&disable_thread_calls));
    }
}
} // namespace ambrosia::services
