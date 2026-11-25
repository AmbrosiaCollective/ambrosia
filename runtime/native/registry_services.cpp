#include "registry_services.h"
#include "guest_strings.h"
#include "service_runtime.h"
#include "startup_trace.h"

namespace ambrosia::services {
namespace {
RegistryKeys &keys() noexcept {
    auto *runtime = current_runtime();
    if (!runtime) {
        diagnostics::stop_trace("registry services require a scoped runtime", 78);
    }
    return runtime->registry_keys();
}

template <class Character> std::u16string key_path(const Character *path) {
    if (!path) {
        return {};
    }
    const auto text = guest::read_string(path);
    // The initial registry implements ASCII case-insensitive names. Stop on
    // wider input instead of silently applying incorrect Unicode casing rules.
    std::u16string result;
    result.reserve(text.size());
    for (const auto character : text) {
        if (static_cast<std::uint32_t>(character) > 127) {
            diagnostics::stop_trace("non-ASCII registry case folding is not implemented", 78);
        }
        result.push_back(static_cast<char16_t>(character));
    }
    return result;
}

template <class Character, bool Legacy>
std::int32_t open_key(void *parent, const Character *path, std::uint32_t options, std::uint32_t access,
                      void **output) noexcept {
    if (!output) {
        return 87;
    }
    try {
        if (options) {
            diagnostics::stop_trace("registry symbolic links/options are not implemented", 78);
        }
        constexpr std::uint32_t read_access = 0x20019;
        constexpr std::uint32_t native_64_view = 0x100;
        if (access & ~(read_access | native_64_view)) {
            diagnostics::stop_trace("registry writes, security changes and 32-bit views are unsupported", 78);
        }
        const auto result = keys().open(reinterpret_cast<std::uintptr_t>(parent), key_path(path), Legacy);
        *output = reinterpret_cast<void *>(result.handle);
        // Registry APIs return LSTATUS directly. They do not use guest errno
        // or thread last-error as a second result channel.
        return static_cast<std::int32_t>(result.error);
    } catch (const std::bad_alloc &) {
        *output = nullptr;
        return 8;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

template <class Character>
std::int32_t AMBROSIA_GUEST_ABI open_legacy(void *parent, const Character *path, void **output) noexcept {
    return open_key<Character, true>(parent, path, 0, 0, output);
}

template <class Character>
std::int32_t AMBROSIA_GUEST_ABI open_extended(void *parent, const Character *path, std::uint32_t options,
                                              std::uint32_t access, void **output) noexcept {
    return open_key<Character, false>(parent, path, options, access, output);
}

std::int32_t AMBROSIA_GUEST_ABI close_key(void *handle) noexcept {
    try {
        return static_cast<std::int32_t>(keys().close(reinterpret_cast<std::uintptr_t>(handle)));
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}
} // namespace

void register_registry_services(ImportRegistry &registry) {
    for (const auto *module :
         {"advapi32.dll", "kernelbase.dll", "api-ms-win-core-registry-l1-1-0.dll",
          "api-ms-win-core-registry-l1-1-1.dll", "api-ms-win-core-registry-l1-1-2.dll"}) {
        registry.add(module, "RegOpenKeyExW", reinterpret_cast<std::uintptr_t>(&open_extended<char16_t>));
        registry.add(module, "RegOpenKeyExA", reinterpret_cast<std::uintptr_t>(&open_extended<char>));
        registry.add(module, "RegCloseKey", reinterpret_cast<std::uintptr_t>(&close_key));
    }
    for (const auto *module :
         {"advapi32.dll", "api-ms-win-core-registry-l2-1-0.dll", "api-ms-win-core-registry-l2-2-0.dll",
          "api-ms-win-core-registry-l2-3-0.dll"}) {
        registry.add(module, "RegOpenKeyW", reinterpret_cast<std::uintptr_t>(&open_legacy<char16_t>));
        registry.add(module, "RegOpenKeyA", reinterpret_cast<std::uintptr_t>(&open_legacy<char>));
    }
}
} // namespace ambrosia::services
