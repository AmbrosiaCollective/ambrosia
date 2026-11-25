#include "locale_services.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include <stdexcept>

namespace ambrosia::services {
namespace {
Runtime &required_runtime() {
    auto *runtime = current_runtime();
    if (!runtime) {
        throw std::runtime_error("CRT locale lock requires a scoped runtime");
    }
    return *runtime;
}

void AMBROSIA_GUEST_ABI lock_locales() noexcept {
    try {
        required_runtime().lock_locales();
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

void AMBROSIA_GUEST_ABI unlock_locales() noexcept {
    try {
        required_runtime().unlock_locales();
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}
} // namespace

void register_locale_services(ImportRegistry &registry) {
    for (const auto *module : {"ucrtbase.dll", "api-ms-win-crt-locale-l1-1-0.dll"}) {
        registry.add(module, "_lock_locales", reinterpret_cast<std::uintptr_t>(&lock_locales));
        registry.add(module, "_unlock_locales", reinterpret_cast<std::uintptr_t>(&unlock_locales));
    }
}
} // namespace ambrosia::services
