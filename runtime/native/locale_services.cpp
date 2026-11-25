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

std::uint32_t AMBROSIA_GUEST_ABI locale_codepage() noexcept {
    return required_runtime().locale().codepage();
}

std::int32_t AMBROSIA_GUEST_ABI multibyte_length() noexcept {
    return required_runtime().locale().max_multibyte_length();
}

char16_t **AMBROSIA_GUEST_ABI locale_names() noexcept {
    return required_runtime().locale().locale_names();
}

const std::uint16_t *AMBROSIA_GUEST_ABI classification() noexcept {
    return required_runtime().locale().classification();
}

char *AMBROSIA_GUEST_ABI select_locale(std::int32_t category, const char *requested) noexcept {
    try {
        auto &runtime = required_runtime();
        runtime.lock_locales();
        // Selection performs no allocation and cannot throw. It shares the
        // same recursive gate as the CRT's explicit locale lock callbacks.
        auto *name = runtime.locale().select(category, requested);
        runtime.unlock_locales();
        if (!name) {
            crt_error() = 22;
        }
        return name;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}
} // namespace

void register_locale_services(ImportRegistry &registry) {
    for (const auto *module : {"ucrtbase.dll", "api-ms-win-crt-locale-l1-1-0.dll"}) {
        registry.add(module, "_lock_locales", reinterpret_cast<std::uintptr_t>(&lock_locales));
        registry.add(module, "_unlock_locales", reinterpret_cast<std::uintptr_t>(&unlock_locales));
        registry.add(module, "setlocale", reinterpret_cast<std::uintptr_t>(&select_locale));
        registry.add(module, "___lc_codepage_func", reinterpret_cast<std::uintptr_t>(&locale_codepage));
        registry.add(module, "___lc_collate_cp_func", reinterpret_cast<std::uintptr_t>(&locale_codepage));
        registry.add(module, "___mb_cur_max_func", reinterpret_cast<std::uintptr_t>(&multibyte_length));
        registry.add(module, "___lc_locale_name_func", reinterpret_cast<std::uintptr_t>(&locale_names));
        registry.add(module, "__pctype_func", reinterpret_cast<std::uintptr_t>(&classification));
    }
}
} // namespace ambrosia::services
