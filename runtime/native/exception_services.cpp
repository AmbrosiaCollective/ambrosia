#include "exception_filter.h"
#include "service_runtime.h"
#include "startup_trace.h"

namespace ambrosia::services {
namespace {
void *AMBROSIA_GUEST_ABI set_unhandled_filter(void *filter) noexcept {
    auto *runtime = current_runtime();
    if (!runtime) {
        diagnostics::stop_trace("exception-filter registration requires a scoped runtime", 78);
    }
    const auto previous = runtime->exception_filter().replace(reinterpret_cast<std::uintptr_t>(filter));
    diagnostics::write_service_trace("TRACE top-level exception filter registered\n");
    return reinterpret_cast<void *>(previous);
}
} // namespace

void register_exception_services(ImportRegistry &registry) {
    for (const auto *module : {"kernel32.dll", "kernelbase.dll", "api-ms-win-core-errorhandling-l1-1-0.dll",
                               "api-ms-win-core-errorhandling-l1-1-1.dll"}) {
        registry.add(module, "SetUnhandledExceptionFilter",
                     reinterpret_cast<std::uintptr_t>(&set_unhandled_filter));
    }
}
} // namespace ambrosia::services
