#include "processor_information.h"
#include "service_runtime.h"
#include "startup_trace.h"

namespace ambrosia::services {
namespace {
std::int32_t AMBROSIA_GUEST_ABI logical_processors(void *buffer, std::uint32_t *bytes) noexcept {
    if (!bytes) {
        set_last_error(87);
        return 0;
    }
    auto *runtime = current_runtime();
    if (!runtime) {
        set_last_error(1);
        return 0;
    }
    try {
        const auto error = runtime->processor_information().copy(buffer, *bytes);
        if (error) {
            set_last_error(error);
            return 0;
        }
        diagnostics::write_service_trace("TRACE logical processor topology accepted\n");
        return 1;
    } catch (const std::bad_alloc &) {
        set_last_error(8);
        return 0;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}
} // namespace

void register_sysinfo_services(ImportRegistry &registry) {
    for (const auto *module : {"kernel32.dll", "kernelbase.dll", "api-ms-win-core-sysinfo-l1-1-0.dll",
                               "api-ms-win-core-sysinfo-l1-2-0.dll", "api-ms-win-core-sysinfo-l1-2-1.dll",
                               "api-ms-win-core-sysinfo-l1-2-2.dll", "api-ms-win-core-sysinfo-l1-2-3.dll"}) {
        registry.add(module, "GetLogicalProcessorInformation",
                     reinterpret_cast<std::uintptr_t>(&logical_processors));
    }
}
} // namespace ambrosia::services
