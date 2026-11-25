#include "kernel_services.h"
#include "guest_strings.h"
#include "service_runtime.h"
#include "startup_trace.h"

namespace ambrosia::services {
namespace {
KernelObjects &objects() noexcept {
    auto *runtime = current_runtime();
    if (!runtime) {
        diagnostics::stop_trace("kernel objects require a scoped runtime", 78);
    }
    return runtime->objects();
}

void *create_event(void *attributes, std::int32_t manual, std::int32_t initial,
                   std::optional<std::u16string> name) {
    if (attributes) {
        diagnostics::stop_trace("event security/inheritance attributes are not implemented", 78);
    }
    if (name && name->size() >= 260) {
        set_last_error(206); // ERROR_FILENAME_EXCED_RANGE; documented MAX_PATH name limit.
        return nullptr;
    }
    if (name && name->starts_with(u"Global\\")) {
        diagnostics::stop_trace("cross-process global event namespaces are unsupported", 78);
    }
    if (name && name->starts_with(u"Local\\")) {
        name->erase(0, 6);
    }
    if (name && name->find(u'\\') != std::u16string::npos) {
        set_last_error(123);
        return nullptr;
    }
    const auto result = objects().create_event(manual != 0, initial != 0, name);
    // Creation distinguishes new objects from an existing named object even
    // when both succeed. SetEvent/ResetEvent/waits have separate error rules.
    set_last_error(result.error);
    return reinterpret_cast<void *>(result.handle);
}

void *AMBROSIA_GUEST_ABI create_event_w(void *attributes, std::int32_t manual, std::int32_t initial,
                                        const char16_t *name) noexcept {
    try {
        return create_event(attributes, manual, initial,
                            name ? std::optional(guest::read_string(name)) : std::nullopt);
    } catch (const std::bad_alloc &) {
        set_last_error(8);
        return nullptr;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

void *AMBROSIA_GUEST_ABI create_event_a(void *attributes, std::int32_t manual, std::int32_t initial,
                                        const char *name) noexcept {
    try {
        std::optional<std::u16string> wide;
        if (name) {
            wide = std::filesystem::path(guest::read_string(name)).u16string();
        }
        return create_event(attributes, manual, initial, std::move(wide));
    } catch (const std::bad_alloc &) {
        set_last_error(8);
        return nullptr;
    } catch (const std::filesystem::filesystem_error &) {
        set_last_error(1113);
        return nullptr;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

template <std::uint32_t (KernelObjects::*Operation)(std::uintptr_t)>
std::int32_t AMBROSIA_GUEST_ABI object_operation(void *handle) noexcept {
    try {
        const auto error = (objects().*Operation)(reinterpret_cast<std::uintptr_t>(handle));
        if (error) {
            set_last_error(error);
        }
        return error ? 0 : 1;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

std::uint32_t AMBROSIA_GUEST_ABI wait_single(void *handle, std::uint32_t milliseconds) noexcept {
    try {
        const auto result = objects().wait(reinterpret_cast<std::uintptr_t>(handle), milliseconds);
        if (result.error) {
            set_last_error(result.error);
        }
        return result.result;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

std::uint32_t AMBROSIA_GUEST_ABI wait_single_ex(void *handle, std::uint32_t milliseconds,
                                                std::int32_t alertable) noexcept {
    if (alertable) {
        diagnostics::stop_trace("alertable waits and guest APC dispatch are not implemented", 78);
    }
    return wait_single(handle, milliseconds);
}
} // namespace

void register_kernel_services(ImportRegistry &registry) {
    for (const auto *module : {"kernel32.dll", "kernelbase.dll", "api-ms-win-core-synch-l1-1-0.dll",
                               "api-ms-win-core-synch-l1-2-0.dll"}) {
        registry.add(module, "CreateEventW", reinterpret_cast<std::uintptr_t>(&create_event_w));
        registry.add(module, "CreateEventA", reinterpret_cast<std::uintptr_t>(&create_event_a));
        registry.add(module, "SetEvent",
                     reinterpret_cast<std::uintptr_t>(&object_operation<&KernelObjects::set_event>));
        registry.add(module, "ResetEvent",
                     reinterpret_cast<std::uintptr_t>(&object_operation<&KernelObjects::reset_event>));
        registry.add(module, "WaitForSingleObject", reinterpret_cast<std::uintptr_t>(&wait_single));
        registry.add(module, "WaitForSingleObjectEx", reinterpret_cast<std::uintptr_t>(&wait_single_ex));
    }
    for (const auto *module : {"kernel32.dll", "kernelbase.dll", "api-ms-win-core-handle-l1-1-0.dll"}) {
        registry.add(module, "CloseHandle",
                     reinterpret_cast<std::uintptr_t>(&object_operation<&KernelObjects::close>));
    }
}
} // namespace ambrosia::services
