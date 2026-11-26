#include "file_services.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include "text_conversion.h"
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

namespace ambrosia::services {
namespace {
struct NativeErrorScope {
    int saved = errno;

    ~NativeErrorScope() {
        errno = saved;
    }
};

std::uint32_t failure(std::uint32_t error) noexcept {
    set_last_error(error);
    return 0xffffffffu;
}

ReadonlyFiles &files() {
    auto *owner = current_runtime();
    if (!owner) {
        diagnostics::stop_trace("file metadata requires a scoped native runtime", 78);
    }
    return owner->files();
}

template <class Unit> std::optional<std::basic_string<Unit>> read_filename(const void *source) {
    std::basic_string<Unit> result;
    const auto *bytes = static_cast<const std::byte *>(source);
    constexpr std::size_t limit = sizeof(Unit) == 1 ? 131069 : 32768;
    for (std::size_t index = 0; index < limit; ++index) {
        Unit unit;
        std::memcpy(&unit, bytes + index * sizeof(Unit), sizeof(unit));
        if (!unit) {
            return result;
        }
        result.push_back(unit);
    }
    return std::nullopt;
}

std::uint32_t inspect(const std::string &path) {
    const auto result = files().attributes(path);
    if (result.error) {
        return failure(result.error);
    }
    diagnostics::write_service_trace("TRACE native file attributes accepted\n");
    return result.value;
}

std::uint32_t AMBROSIA_GUEST_ABI attributes_wide(const void *source) noexcept {
    NativeErrorScope restore;
    if (!source) {
        return failure(87);
    }
    try {
        const auto path = read_filename<char16_t>(source);
        if (!path) {
            return failure(206);
        }
        const auto utf8 = unicode::utf16_to_utf8(*path, true);
        if (!utf8) {
            return failure(123);
        }
        return inspect(*utf8);
    } catch (const std::bad_alloc &) {
        return failure(8);
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

std::uint32_t AMBROSIA_GUEST_ABI attributes_bytes(const char *source) noexcept {
    NativeErrorScope restore;
    if (!source) {
        return failure(87);
    }
    try {
        const auto path = read_filename<char>(source);
        if (!path) {
            return failure(206);
        }
        return inspect(*path);
    } catch (const std::bad_alloc &) {
        return failure(8);
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}
} // namespace

void register_file_services(ImportRegistry &registry) {
    for (const auto *module : {"kernel32.dll", "kernelbase.dll", "api-ms-win-core-file-l1-1-0.dll",
                               "api-ms-win-core-file-l1-2-0.dll"}) {
        registry.add(module, "GetFileAttributesW", reinterpret_cast<std::uintptr_t>(&attributes_wide));
        registry.add(module, "GetFileAttributesA", reinterpret_cast<std::uintptr_t>(&attributes_bytes));
    }
}
} // namespace ambrosia::services
