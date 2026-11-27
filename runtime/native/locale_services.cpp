#include "locale_services.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include <cerrno>
#include <cstring>
#include <optional>
#include <stdexcept>

namespace ambrosia::services {
namespace {
struct RestoreNativeError {
    int saved = errno;

    ~RestoreNativeError() {
        errno = saved;
    }
};

Runtime &required_runtime() {
    auto *runtime = current_runtime();
    if (!runtime) {
        throw std::runtime_error("CRT locale lock requires a scoped runtime");
    }
    return *runtime;
}

std::int32_t AMBROSIA_GUEST_ABI user_geographic_name(void *output, std::int32_t capacity) noexcept {
    const auto saved_error = errno;
    // No user geography is exposed by the isolated runtime. UN M49 "001"
    // represents the world, never an inferred country or machine location.
    constexpr char16_t world[] = u"001";
    constexpr std::int32_t required = sizeof(world) / sizeof(world[0]);
    std::int32_t result = 0;
    if (capacity < 0 || (!output && capacity)) {
        set_last_error(error_invalid_parameter);
    } else if (!capacity) {
        result = required;
    } else if (capacity < required) {
        set_last_error(122);
    } else {
        std::memcpy(output, world, sizeof(world));
        result = required;
    }
    errno = saved_error;
    return result;
}

std::int32_t AMBROSIA_GUEST_ABI user_locale_name(void *output, std::int32_t capacity) noexcept {
    RestoreNativeError restore;
    try {
        auto *runtime = current_runtime();
        if (!runtime) {
            throw std::runtime_error("user locale requires a scoped runtime");
        }
        const auto name = runtime->user_locale().name();
        const auto required = static_cast<std::int32_t>(name.size() + 1);
        if (!output || capacity < required) {
            set_last_error(122);
            return 0;
        }
        std::memcpy(output, name.c_str(), static_cast<std::size_t>(required) * sizeof(char16_t));
        diagnostics::write_service_trace("TRACE native guest user locale returned\n");
        return required;
    } catch (const std::bad_alloc &) {
        set_last_error(8);
        return 0;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

std::optional<std::string> locale_input(const void *source) {
    std::string result;
    const auto *bytes = static_cast<const std::byte *>(source);
    for (std::size_t index = 0; index < UserLocale::name_capacity; ++index) {
        char16_t unit;
        std::memcpy(&unit, bytes + index * sizeof(unit), sizeof(unit));
        if (!unit) {
            return result;
        }
        if (unit > 127) {
            return std::nullopt;
        }
        result.push_back(static_cast<char>(unit));
    }
    return std::nullopt;
}

std::uint32_t AMBROSIA_GUEST_ABI locale_to_identifier(const void *source, std::uint32_t flags) noexcept {
    RestoreNativeError restore;
    constexpr std::uint32_t allow_neutral = 0x08000000;
    try {
        if (flags & ~allow_neutral) {
            set_last_error(87);
            return 0;
        }
        std::optional<std::string> name;
        if (source) {
            name = locale_input(source);
        } else {
            const auto selected = required_runtime().user_locale().name();
            name = std::string(selected.begin(), selected.end());
        }
        if (!name) {
            set_last_error(87);
            return 0;
        }
        if (name->empty()) {
            return 0x007f; // LOCALE_INVARIANT, independent of the guest preference.
        }
        if (*name == "!x-sys-default-locale" || name->find('_') != std::string::npos) {
            throw std::runtime_error("system-default and legacy Windows sort locale names are unsupported");
        }
        const auto identifier = lookup_locale_identifier(*name);
        if (!identifier) {
            throw std::runtime_error("locale has no supported lossless Windows LCID mapping");
        }
        if (identifier->neutral && !(flags & allow_neutral)) {
            throw std::runtime_error("neutral-to-specific Windows locale inference is unsupported");
        }
        diagnostics::write_service_trace("TRACE native locale identifier resolved\n");
        return identifier->value;
    } catch (const std::invalid_argument &) {
        set_last_error(87);
        return 0;
    } catch (const std::bad_alloc &) {
        set_last_error(8);
        return 0;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
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

CrtLocale &required_locale() noexcept {
    try {
        return required_runtime().locale();
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

std::int32_t AMBROSIA_GUEST_ABI configure_thread_locale(std::int32_t mode) noexcept {
    try {
        const auto previous = required_runtime().configure_thread_locale(mode);
        if (previous == -1) {
            crt_error() = 22;
        }
        return previous;
    } catch (const std::bad_alloc &) {
        crt_error() = 12;
        return -1;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

std::uint32_t AMBROSIA_GUEST_ABI locale_codepage() noexcept {
    return required_locale().codepage();
}

std::int32_t AMBROSIA_GUEST_ABI multibyte_length() noexcept {
    return required_locale().max_multibyte_length();
}

char16_t **AMBROSIA_GUEST_ABI locale_names() noexcept {
    return required_locale().locale_names();
}

const std::uint16_t *AMBROSIA_GUEST_ABI classification() noexcept {
    return required_locale().classification();
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
    for (const auto *module : {"kernel32.dll", "kernelbase.dll", "api-ms-win-core-localization-l1-2-0.dll"}) {
        registry.add(module, "GetUserDefaultLocaleName", reinterpret_cast<std::uintptr_t>(&user_locale_name));
        registry.add(module, "LocaleNameToLCID", reinterpret_cast<std::uintptr_t>(&locale_to_identifier));
    }

    for (const auto *module : {"kernel32.dll", "kernelbase.dll", "api-ms-win-core-localization-l1-2-3.dll"}) {
        registry.add(module, "GetUserDefaultGeoName",
                     reinterpret_cast<std::uintptr_t>(&user_geographic_name));
    }

    for (const auto *module : {"ucrtbase.dll", "api-ms-win-crt-locale-l1-1-0.dll"}) {
        registry.add(module, "_lock_locales", reinterpret_cast<std::uintptr_t>(&lock_locales));
        registry.add(module, "_unlock_locales", reinterpret_cast<std::uintptr_t>(&unlock_locales));
        registry.add(module, "setlocale", reinterpret_cast<std::uintptr_t>(&select_locale));
        registry.add(module, "___lc_codepage_func", reinterpret_cast<std::uintptr_t>(&locale_codepage));
        registry.add(module, "___lc_collate_cp_func", reinterpret_cast<std::uintptr_t>(&locale_codepage));
        registry.add(module, "___mb_cur_max_func", reinterpret_cast<std::uintptr_t>(&multibyte_length));
        registry.add(module, "___lc_locale_name_func", reinterpret_cast<std::uintptr_t>(&locale_names));
        registry.add(module, "__pctype_func", reinterpret_cast<std::uintptr_t>(&classification));
        registry.add(module, "_configthreadlocale",
                     reinterpret_cast<std::uintptr_t>(&configure_thread_locale));
    }
}
} // namespace ambrosia::services
