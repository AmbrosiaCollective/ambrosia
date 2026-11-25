#include "graphics_services.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include <array>
#include <cstring>

namespace ambrosia::services {
namespace {
std::int32_t AMBROSIA_GUEST_ABI configure_memory(const void *description) noexcept {
    if (!description) {
        return static_cast<std::int32_t>(0x80004003u); // E_POINTER.
    }
    try {
        auto *runtime = current_runtime();
        if (!runtime) {
            diagnostics::stop_trace("graphics memory policy requires a scoped runtime", 78);
        }
        // Only the observed 32-byte caller profile is supported. Copy once;
        // validation never writes guest input or retains its stack pointer.
        std::array<std::byte, 32> copy{};
        std::memcpy(copy.data(), description, copy.size());
        const auto result = runtime->graphics_memory().configure(copy);
        diagnostics::write_service_trace(result == 0 ? "TRACE graphics-memory configuration accepted\n"
                                                     : "TRACE graphics-memory configuration rejected\n");
        return result;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}
} // namespace

void register_graphics_services(ImportRegistry &registry) {
    registry.add("d3d12_xs.dll", "D3D12ConfigureVirtualMemoryX",
                 reinterpret_cast<std::uintptr_t>(&configure_memory));
}
} // namespace ambrosia::services
