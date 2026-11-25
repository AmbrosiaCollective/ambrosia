#include "pointer_codec.h"
#include "service_runtime.h"
#include "startup_trace.h"

namespace ambrosia::services {
namespace {
const PointerCodec &codec() noexcept {
    auto *runtime = current_runtime();
    if (!runtime) {
        diagnostics::stop_trace("pointer encoding requires a scoped runtime", 78);
    }
    return runtime->pointers();
}

void *AMBROSIA_GUEST_ABI encode(void *pointer) noexcept {
    return reinterpret_cast<void *>(codec().encode(reinterpret_cast<std::uintptr_t>(pointer)));
}

void *AMBROSIA_GUEST_ABI decode(void *encoded) noexcept {
    return reinterpret_cast<void *>(codec().decode(reinterpret_cast<std::uintptr_t>(encoded)));
}
} // namespace

void register_pointer_services(ImportRegistry &registry) {
    for (const auto *module : {"kernel32.dll", "kernelbase.dll", "api-ms-win-core-util-l1-1-0.dll"}) {
        registry.add(module, "EncodePointer", reinterpret_cast<std::uintptr_t>(&encode));
        registry.add(module, "DecodePointer", reinterpret_cast<std::uintptr_t>(&decode));
    }
}
} // namespace ambrosia::services
