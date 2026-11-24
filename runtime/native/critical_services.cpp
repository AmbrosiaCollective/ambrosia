#include "critical_services.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include <stdexcept>

namespace ambrosia::services {
namespace {
std::int32_t AMBROSIA_GUEST_ABI initialize_ex(void *address, std::uint32_t spin,
                                              std::uint32_t flags) noexcept {
    auto *runtime = current_runtime();
    const auto error =
        runtime ? runtime->critical_sections().initialize(address, spin, flags) : error_invalid_function;
    if (error) {
        set_last_error(error);
    }
    return error ? 0 : 1;
}

std::int32_t AMBROSIA_GUEST_ABI initialize_spin(void *address, std::uint32_t spin) noexcept {
    return initialize_ex(address, spin, 0);
}

void AMBROSIA_GUEST_ABI initialize(void *address) noexcept {
    if (!initialize_ex(address, 0, 0)) {
        diagnostics::stop_trace("critical-section initialization failed", 78);
    }
}

CriticalSections &required_sections() {
    auto *runtime = current_runtime();
    if (!runtime) {
        throw std::runtime_error("critical-section operation requires an active runtime");
    }
    return runtime->critical_sections();
}

void AMBROSIA_GUEST_ABI enter(void *address) noexcept {
    try {
        (void)required_sections().enter(address);
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

std::int32_t AMBROSIA_GUEST_ABI try_enter(void *address) noexcept {
    try {
        return required_sections().enter(address, true) ? 1 : 0;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

void AMBROSIA_GUEST_ABI leave(void *address) noexcept {
    try {
        required_sections().leave(address);
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

void AMBROSIA_GUEST_ABI remove(void *address) noexcept {
    try {
        required_sections().remove(address);
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

std::uint32_t AMBROSIA_GUEST_ABI spin_count(void *address, std::uint32_t spin) noexcept {
    try {
        return required_sections().set_spin(address, spin);
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}
} // namespace

void register_critical_services(ImportRegistry &registry) {
    for (const auto *module : {"kernel32.dll", "kernelbase.dll", "api-ms-win-core-synch-l1-1-0.dll"}) {
        registry.add(module, "InitializeCriticalSectionEx", reinterpret_cast<std::uintptr_t>(&initialize_ex));
        registry.add(module, "InitializeCriticalSectionAndSpinCount",
                     reinterpret_cast<std::uintptr_t>(&initialize_spin));
        registry.add(module, "InitializeCriticalSection", reinterpret_cast<std::uintptr_t>(&initialize));
        registry.add(module, "EnterCriticalSection", reinterpret_cast<std::uintptr_t>(&enter));
        registry.add(module, "TryEnterCriticalSection", reinterpret_cast<std::uintptr_t>(&try_enter));
        registry.add(module, "LeaveCriticalSection", reinterpret_cast<std::uintptr_t>(&leave));
        registry.add(module, "DeleteCriticalSection", reinterpret_cast<std::uintptr_t>(&remove));
        registry.add(module, "SetCriticalSectionSpinCount", reinterpret_cast<std::uintptr_t>(&spin_count));
    }
}
} // namespace ambrosia::services
