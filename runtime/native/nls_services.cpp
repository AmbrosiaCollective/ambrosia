#include "nls_services.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include "text_conversion.h"
#include <cerrno>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>

namespace ambrosia::services {
namespace {
struct NativeErrorScope {
    int saved = errno;

    ~NativeErrorScope() {
        errno = saved;
    }
};

std::int32_t failure(std::uint32_t error) noexcept {
    set_last_error(error);
    return 0;
}

void require_utf8(std::uint32_t codepage, std::uint32_t flags) {
    if (codepage != 0 && codepage != native_ansi_codepage) {
        diagnostics::stop_trace("unsupported native text codepage=" + std::to_string(codepage) +
                                    " flags=" + std::to_string(flags),
                                78);
    }
}

template <class Unit> std::optional<std::size_t> source_length(const void *source, std::int32_t count) {
    if (count > text_input_limit) {
        return std::nullopt;
    }
    if (count > 0) {
        return static_cast<std::size_t>(count);
    }
    const auto *bytes = static_cast<const std::byte *>(source);
    for (std::int32_t index = 0; index < text_input_limit; ++index) {
        Unit unit;
        std::memcpy(&unit, bytes + static_cast<std::size_t>(index) * sizeof(Unit), sizeof(unit));
        if (!unit) {
            return static_cast<std::size_t>(index) + 1;
        }
    }
    return std::nullopt;
}

template <class Unit>
std::int32_t publish(const std::basic_string<Unit> &converted, void *output, std::int32_t capacity) {
    const auto required = static_cast<std::int32_t>(converted.size());
    if (!capacity) {
        return required;
    }
    if (capacity < required) {
        return failure(122);
    }
    // Copy only converted units. ICU's optional convenience terminator must
    // never leak into a Windows explicit-count result or adjacent guest field.
    std::memcpy(output, converted.data(), converted.size() * sizeof(Unit));
    return required;
}

std::uint32_t AMBROSIA_GUEST_ABI ansi_codepage() noexcept {
    return native_ansi_codepage;
}

std::int32_t AMBROSIA_GUEST_ABI to_wide(std::uint32_t codepage, std::uint32_t flags, const char *source,
                                        std::int32_t count, void *output, std::int32_t capacity) noexcept {
    NativeErrorScope restore;
    require_utf8(codepage, flags);
    if (flags & ~8u) {
        return failure(1004);
    }
    if (!source || count == 0 || count < -1 || capacity < 0 || (!output && capacity) || source == output) {
        return failure(87);
    }
    try {
        const auto length = source_length<char>(source, count);
        if (!length) {
            return failure(8);
        }
        const auto converted = unicode::utf8_to_utf16(std::string_view(source, *length), (flags & 8u) != 0);
        if (!converted) {
            return failure(1113);
        }
        return publish(*converted, output, capacity);
    } catch (const std::bad_alloc &) {
        return failure(8);
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

std::int32_t AMBROSIA_GUEST_ABI to_bytes(std::uint32_t codepage, std::uint32_t flags, const void *source,
                                         std::int32_t count, void *output, std::int32_t capacity,
                                         const char *default_character, void *used_default) noexcept {
    NativeErrorScope restore;
    require_utf8(codepage, flags);
    if (flags & ~128u) {
        return failure(1004);
    }
    if (!source || count == 0 || count < -1 || capacity < 0 || (!output && capacity) || source == output ||
        default_character || used_default) {
        return failure(87);
    }
    try {
        const auto length = source_length<char16_t>(source, count);
        if (!length) {
            return failure(8);
        }
        std::u16string aligned(*length, u'\0');
        std::memcpy(aligned.data(), source, *length * sizeof(char16_t));
        const auto converted = unicode::utf16_to_utf8(aligned, (flags & 128u) != 0);
        if (!converted) {
            return failure(1113);
        }
        return publish(*converted, output, capacity);
    } catch (const std::bad_alloc &) {
        return failure(8);
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}
} // namespace

void register_nls_services(ImportRegistry &registry) {
    for (const auto *module :
         {"kernel32.dll", "kernelbase.dll", "api-ms-win-core-string-l1-1-0.dll",
          "api-ms-win-core-string-l2-1-0.dll", "api-ms-win-core-localization-l1-2-0.dll"}) {
        registry.add(module, "MultiByteToWideChar", reinterpret_cast<std::uintptr_t>(&to_wide));
        registry.add(module, "WideCharToMultiByte", reinterpret_cast<std::uintptr_t>(&to_bytes));
        registry.add(module, "GetACP", reinterpret_cast<std::uintptr_t>(&ansi_codepage));
    }
}
} // namespace ambrosia::services
