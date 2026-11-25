#include "loader_services.h"
#include "guest_strings.h"
#include "module_graph.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include <cstring>
#include <stdexcept>

namespace ambrosia::services {
namespace {
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
    auto base = modules->loaded_module(name);
    if (!base && name && runtime->has_service_module(*name)) {
        base = runtime->native_modules().handle(*name);
    }
    if (!base) {
        set_last_error(126);
    }
    return reinterpret_cast<void *>(base);
}

void *AMBROSIA_GUEST_ABI module_handle_a(const char *name) noexcept {
    try {
        return loaded_handle(name ? std::optional(guest::read_string(name)) : std::nullopt);
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
        const auto utf8 = std::filesystem::path(guest::read_string(name)).u8string();
        return loaded_handle(std::string(reinterpret_cast<const char *>(utf8.data()), utf8.size()));
    } catch (const std::bad_alloc &) {
        set_last_error(8);
        return nullptr;
    } catch (const std::exception &) {
        set_last_error(123);
        return nullptr;
    }
}

void *AMBROSIA_GUEST_ABI procedure_address(void *module, const char *function) noexcept {
    try {
        auto *runtime = current_runtime();
        auto *modules = current_modules();
        if (!runtime || !modules) {
            diagnostics::stop_trace("dynamic exports require a scoped runtime and graph", 78);
        }
        if (!module) {
            set_last_error(6);
            return nullptr;
        }
        const auto handle = reinterpret_cast<std::uintptr_t>(module);
        pe::Import symbol;
        const auto identifier = reinterpret_cast<std::uintptr_t>(function);
        std::optional<std::uintptr_t> address;
        if (identifier <= 65535) {
            if (runtime->native_modules().module_base_for_address(handle) == handle) {
                diagnostics::stop_trace("native service public ordinal compatibility is unsupported", 78);
            }
            symbol.by_ordinal = true;
            symbol.ordinal = static_cast<std::uint16_t>(identifier);
        } else {
            symbol.name = guest::read_string(function);
            address = runtime->native_modules().export_address(handle, symbol.name);
        }
        if (!address.has_value()) {
            // Forwarders can reach loaded images or the existing native service
            // resolver, but this path never invokes the package/file provider.
            address = modules->loaded_export(handle, symbol);
        }
        if (!address.has_value()) {
            set_last_error(6);
            return nullptr;
        }
        if (!*address) {
            set_last_error(127);
        }
        return reinterpret_cast<void *>(*address);
    } catch (const std::bad_alloc &) {
        set_last_error(8);
        return nullptr;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
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
        auto base_address = modules->module_base_for_address(reinterpret_cast<std::uintptr_t>(address));
        if (!base_address) {
            if (auto *runtime = current_runtime()) {
                base_address = runtime->native_modules().module_base_for_address(
                    reinterpret_cast<std::uintptr_t>(address));
            }
        }
        auto *base = reinterpret_cast<void *>(base_address);
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
        registry.add(module, "GetProcAddress", reinterpret_cast<std::uintptr_t>(&procedure_address));
    }
}
} // namespace ambrosia::services
