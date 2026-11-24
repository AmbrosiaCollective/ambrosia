#include "crt_streams.h"
#include "service_runtime.h"
#include "startup_trace.h"

namespace ambrosia::services {
namespace {
void *AMBROSIA_GUEST_ABI standard_stream(unsigned index) noexcept {
    auto *runtime = current_runtime();
    if (!runtime) {
        diagnostics::stop_trace("CRT stream accessed without scoped runtime", 78);
    }
    auto *file = runtime->streams().file(index);
    if (!file) {
        diagnostics::stop_trace("unsupported CRT standard-stream index", 78);
    }
    return file;
}
} // namespace

void register_crt_streams(ImportRegistry &registry) {
    for (const auto *module : {"ucrtbase.dll", "api-ms-win-crt-stdio-l1-1-0.dll"}) {
        registry.add(module, "__acrt_iob_func", reinterpret_cast<std::uintptr_t>(&standard_stream));
    }
}
} // namespace ambrosia::services
