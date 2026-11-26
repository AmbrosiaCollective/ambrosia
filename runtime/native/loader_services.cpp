#include "loader_services.h"
#include "guest_strings.h"
#include "module_graph.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include <algorithm>
#include <cerrno>
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

void *load_library(std::string name, void *file, std::uint32_t flags) {
    auto *runtime = current_runtime();
    auto *modules = current_modules();
    if (!runtime || !modules) {
        diagnostics::stop_trace("dynamic libraries require a scoped runtime and graph", 78);
    }
    if (file) {
        set_last_error(87);
        return nullptr;
    }
    if (flags != 0 && flags != 0x800 && flags != 0x1000) {
        diagnostics::stop_trace("unsupported native library loading flags", 78);
    }
    while (!name.empty() && name.back() == ' ') {
        name.pop_back();
    }
    if (name.empty()) {
        set_last_error(126);
        return nullptr;
    }
    if (name.find_first_of("/\\:") != std::string::npos) {
        diagnostics::stop_trace("full-path dynamic module loading is unsupported", 78);
    }
    if (name.back() == '.') {
        diagnostics::stop_trace("extension-suppressed native library names are unsupported", 78);
    }
    const auto key = ImportRegistry::module_name(std::move(name));
    auto handle = modules->loaded_module(key);
    if (!handle && runtime->has_service_module(key)) {
        handle = runtime->native_modules().handle(key);
    }
    if (!handle) {
        if (flags != 0x800 && modules->has_available_source(key)) {
            diagnostics::stop_trace("new package module initialization during execution is unsupported", 78);
        }
        diagnostics::write_service_trace("TRACE native library unavailable: " + key + "\n");
        set_last_error(126);
        return nullptr;
    }
    if (!runtime->library_references().retain(handle)) {
        set_last_error(8);
        return nullptr;
    }
    return reinterpret_cast<void *>(handle);
}

void *AMBROSIA_GUEST_ABI load_library_ex_w(const char16_t *name, void *file, std::uint32_t flags) noexcept {
    const auto saved_error = errno;
    void *result = nullptr;
    try {
        if (!name) {
            set_last_error(87);
        } else {
            const auto text = std::filesystem::path(guest::read_string(name)).u8string();
            result = load_library(std::string(reinterpret_cast<const char *>(text.data()), text.size()), file,
                                  flags);
        }
    } catch (const std::bad_alloc &) {
        set_last_error(8);
    } catch (const std::exception &) {
        set_last_error(123);
    }
    errno = saved_error;
    return result;
}

void *AMBROSIA_GUEST_ABI load_library_ex_a(const char *name, void *file, std::uint32_t flags) noexcept {
    const auto saved_error = errno;
    void *result = nullptr;
    try {
        if (!name) {
            set_last_error(87);
        } else {
            result = load_library(guest::read_string(name), file, flags);
        }
    } catch (const std::bad_alloc &) {
        set_last_error(8);
    } catch (const std::exception &) {
        set_last_error(123);
    }
    errno = saved_error;
    return result;
}

void *AMBROSIA_GUEST_ABI load_library_w(const char16_t *name) noexcept {
    return load_library_ex_w(name, nullptr, 0);
}

void *AMBROSIA_GUEST_ABI load_library_a(const char *name) noexcept {
    return load_library_ex_a(name, nullptr, 0);
}

std::int32_t AMBROSIA_GUEST_ABI free_library(void *module) noexcept {
    try {
        auto *runtime = current_runtime();
        if (!runtime) {
            diagnostics::stop_trace("library reference release requires a scoped runtime", 78);
        }
        if (!runtime->library_references().release(reinterpret_cast<std::uintptr_t>(module))) {
            set_last_error(6);
            return 0;
        }
        // Static graph imports and the native catalog pin their images through
        // process cleanup; balancing an extra reference cannot detach them.
        return 1;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
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

template <class Character>
std::uint32_t module_filename(void *module, Character *output, std::uint32_t size) noexcept {
    const auto saved_error = errno;

    // Every returning path restores native errno, including conversion/allocation
    // failures. Guest Win32 errors remain independent of CRT/native errno.
    struct Restore {
        int value;

        ~Restore() {
            errno = value;
        }
    } restore{saved_error};

    try {
        auto *modules = current_modules();
        auto *runtime = current_runtime();
        if (!modules || !runtime) {
            diagnostics::stop_trace("module filename queries require a scoped runtime and graph", 78);
        }
        if (!size) {
            set_last_error(122);
            return 0;
        }
        if (!output) {
            diagnostics::stop_trace("module filename query requires a destination buffer", 78);
        }
        const auto handle = reinterpret_cast<std::uintptr_t>(module);
        const auto path = modules->loaded_module_path(handle);
        if (!path) {
            if (handle && runtime->native_modules().module_base_for_address(handle) == handle) {
                diagnostics::stop_trace("native service facades have no file-backed source path", 78);
            }
            set_last_error(126);
            return 0;
        }
        const auto name = [&] {
            if constexpr (sizeof(Character) == 2) {
                return path->u16string();
            } else {
                const auto utf8 = path->u8string();
                return std::string(reinterpret_cast<const char *>(utf8.data()), utf8.size());
            }
        }();
        const auto copied = std::min(name.size(), static_cast<std::size_t>(size - 1));
        std::memcpy(output, name.data(), copied * sizeof(Character));
        const Character terminator = 0;
        std::memcpy(reinterpret_cast<std::byte *>(output) + copied * sizeof(Character), &terminator,
                    sizeof(terminator));
        if (name.size() >= size) {
            set_last_error(122); // Modern Windows truncates, terminates and returns capacity.
            return size;
        }
        return static_cast<std::uint32_t>(name.size());
    } catch (const std::bad_alloc &) {
        set_last_error(8);
        return 0;
    } catch (const std::filesystem::filesystem_error &) {
        set_last_error(1113);
        return 0;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

std::uint32_t AMBROSIA_GUEST_ABI module_filename_w(void *module, char16_t *output,
                                                   std::uint32_t size) noexcept {
    return module_filename(module, output, size);
}

std::uint32_t AMBROSIA_GUEST_ABI module_filename_a(void *module, char *output, std::uint32_t size) noexcept {
    return module_filename(module, output, size);
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
            symbol.by_ordinal = true;
            symbol.ordinal = static_cast<std::uint16_t>(identifier);
            address = runtime->native_modules().export_address(handle, symbol.ordinal);
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
        registry.add(module, "GetModuleFileNameW", reinterpret_cast<std::uintptr_t>(&module_filename_w));
        registry.add(module, "GetModuleFileNameA", reinterpret_cast<std::uintptr_t>(&module_filename_a));
        registry.add(module, "LoadLibraryExW", reinterpret_cast<std::uintptr_t>(&load_library_ex_w));
        registry.add(module, "LoadLibraryExA", reinterpret_cast<std::uintptr_t>(&load_library_ex_a));
        registry.add(module, "LoadLibraryW", reinterpret_cast<std::uintptr_t>(&load_library_w));
        registry.add(module, "LoadLibraryA", reinterpret_cast<std::uintptr_t>(&load_library_a));
        registry.add(module, "FreeLibrary", reinterpret_cast<std::uintptr_t>(&free_library));
        registry.add(module, "GetProcAddress", reinterpret_cast<std::uintptr_t>(&procedure_address));
    }
}
} // namespace ambrosia::services
