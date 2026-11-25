#include "loader_services.h"
#include "module_graph.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include <cstring>
#include <stdexcept>

namespace ambrosia::services {
namespace {
template <class Character> std::basic_string<Character> module_name(const Character *source) {
    std::basic_string<Character> name;
    for (std::size_t index = 0; index < 32768; ++index) {
        const auto character = source[index];
        if (!character) {
            return name;
        }
        name.push_back(character);
    }
    throw std::runtime_error("module name exceeds the runtime string budget");
}

void *loaded_handle(std::optional<std::string> name) {
    auto *modules = current_modules();
    auto *runtime = current_runtime();
    if (!modules || !runtime) {
        diagnostics::stop_trace("module handles require a scoped runtime and graph", 78);
    }
    if (name && name->empty()) {
        set_last_error(126);
        return nullptr;
    }
    if (name && name->find_first_of("/\\:") != std::string::npos) {
        diagnostics::stop_trace("full-path module handle queries are not implemented", 78);
    }
    const auto base = modules->loaded_module(name);
    if (!base && name && runtime->has_service_module(*name)) {
        // Native ABI adapters are not mapped PE DLLs. Do not expose an opaque
        // token as a fake PE base or pretend that a registered service is absent.
        diagnostics::stop_trace("native service module handles are not implemented", 78);
    }
    if (!base) {
        set_last_error(126);
    }
    return reinterpret_cast<void *>(base);
}

void *AMBROSIA_GUEST_ABI module_handle_a(const char *name) noexcept {
    try {
        return loaded_handle(name ? std::optional(module_name(name)) : std::nullopt);
    } catch (const std::bad_alloc &) {
        set_last_error(8);
        return nullptr;
    } catch (const std::exception &) {
        set_last_error(123);
        return nullptr;
    }
}

void *AMBROSIA_GUEST_ABI module_handle_w(const char16_t *name) noexcept {
    try {
        if (!name) {
            return loaded_handle(std::nullopt);
        }
        const auto utf8 = std::filesystem::path(module_name(name)).u8string();
        return loaded_handle(std::string(reinterpret_cast<const char *>(utf8.data()), utf8.size()));
    } catch (const std::bad_alloc &) {
        set_last_error(8);
        return nullptr;
    } catch (const std::exception &) {
        set_last_error(123);
        return nullptr;
    }
}

std::int32_t AMBROSIA_GUEST_ABI disable_thread_calls(void *module) noexcept {
    try {
        auto *modules = current_modules();
        if (modules && modules->disable_thread_notifications(reinterpret_cast<std::uintptr_t>(module))) {
            return 1;
        }
    } catch (...) {
        // The module graph enforces its host-thread owner and valid lifetime.
    }
    set_last_error(126); // ERROR_MOD_NOT_FOUND, including DLLs with active static TLS.
    return 0;
}

void *AMBROSIA_GUEST_ABI pc_to_file_header(void *address, void *base_output) noexcept {
    try {
        auto *modules = current_modules();
        if (!modules || !base_output) {
            throw std::runtime_error("PC-to-image lookup requires a scoped graph and output pointer");
        }
        auto *base = reinterpret_cast<void *>(
            modules->module_base_for_address(reinterpret_cast<std::uintptr_t>(address)));
        std::memcpy(base_output, &base, sizeof(base));
        return base;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}
} // namespace

void register_loader_services(ImportRegistry &registry) {
    for (const auto *module : {"ntdll.dll", "api-ms-win-core-rtlsupport-l1-1-0.dll"}) {
        registry.add(module, "RtlPcToFileHeader", reinterpret_cast<std::uintptr_t>(&pc_to_file_header));
    }
    for (const auto *module :
         {"kernel32.dll", "kernelbase.dll", "api-ms-win-core-libraryloader-l1-1-0.dll",
          "api-ms-win-core-libraryloader-l1-1-1.dll", "api-ms-win-core-libraryloader-l1-2-0.dll",
          "api-ms-win-core-libraryloader-l1-2-1.dll", "api-ms-win-core-libraryloader-l1-2-2.dll",
          "api-ms-win-core-libraryloader-l1-2-3.dll"}) {
        registry.add(module, "DisableThreadLibraryCalls",
                     reinterpret_cast<std::uintptr_t>(&disable_thread_calls));
        registry.add(module, "GetModuleHandleA", reinterpret_cast<std::uintptr_t>(&module_handle_a));
        registry.add(module, "GetModuleHandleW", reinterpret_cast<std::uintptr_t>(&module_handle_w));
    }
}
} // namespace ambrosia::services
