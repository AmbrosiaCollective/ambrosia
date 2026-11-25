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

std::uint32_t normalize_object_name(std::optional<std::u16string> &name) {
    if (name && name->size() >= 260) {
        return 206; // ERROR_FILENAME_EXCED_RANGE; documented MAX_PATH name limit.
    }
    if (name && name->starts_with(u"Global\\")) {
        diagnostics::stop_trace("cross-process global kernel-object namespaces are unsupported", 78);
    }
    if (name && name->starts_with(u"Local\\")) {
        name->erase(0, 6);
    }
    if (name && name->find(u'\\') != std::u16string::npos) {
        return 123;
    }
    return 0;
}

void *create_event(void *attributes, std::int32_t manual, std::int32_t initial,
                   std::optional<std::u16string> name) {
    if (attributes) {
        diagnostics::stop_trace("event security/inheritance attributes are not implemented", 78);
    }
    if (const auto error = normalize_object_name(name)) {
        set_last_error(error);
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

void *create_mutex(void *attributes, std::int32_t initial_owner, std::optional<std::u16string> name) {
    if (attributes) {
        diagnostics::stop_trace("mutex security/inheritance attributes are not implemented", 78);
    }
    if (const auto error = normalize_object_name(name)) {
        set_last_error(error);
        return nullptr;
    }
    const auto result = objects().create_mutex(initial_owner != 0, name);
    set_last_error(result.error);
    return reinterpret_cast<void *>(result.handle);
}

void *AMBROSIA_GUEST_ABI create_mutex_w(void *attributes, std::int32_t initial_owner,
                                        const char16_t *name) noexcept {
    try {
        return create_mutex(attributes, initial_owner,
                            name ? std::optional(guest::read_string(name)) : std::nullopt);
    } catch (const std::bad_alloc &) {
        set_last_error(8);
        return nullptr;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

void *AMBROSIA_GUEST_ABI create_mutex_a(void *attributes, std::int32_t initial_owner,
                                        const char *name) noexcept {
    try {
        auto wide =
            name ? std::optional(std::filesystem::path(guest::read_string(name)).u16string()) : std::nullopt;
        return create_mutex(attributes, initial_owner, std::move(wide));
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

std::uint32_t semaphore_access(std::uint32_t desired) {
    auto access = desired & ~0xf0000000u;
    if (desired & 0x80000000u) {
        access |= 0x20001; // GENERIC_READ: READ_CONTROL | SEMAPHORE_QUERY_STATE.
    }
    if (desired & 0x40000000u) {
        access |= 0x20002; // GENERIC_WRITE: READ_CONTROL | SEMAPHORE_MODIFY_STATE.
    }
    if (desired & 0x20000000u) {
        access |= 0x120000; // GENERIC_EXECUTE: READ_CONTROL | SYNCHRONIZE.
    }
    if (desired & 0x10000000u) {
        access |= object_all_access;
    }
    return access;
}

void *AMBROSIA_GUEST_ABI create_semaphore_ex_w(void *attributes, std::int32_t initial, std::int32_t maximum,
                                               const char16_t *name, std::uint32_t flags,
                                               std::uint32_t desired) noexcept {
    try {
        if (attributes) {
            diagnostics::stop_trace("semaphore security/inheritance attributes are not implemented", 78);
        }
        if (flags) {
            set_last_error(87);
            return nullptr;
        }
        auto wide = name ? std::optional(guest::read_string(name)) : std::nullopt;
        if (const auto error = normalize_object_name(wide)) {
            set_last_error(error);
            return nullptr;
        }
        const auto result = objects().create_semaphore(initial, maximum, wide, semaphore_access(desired));
        set_last_error(result.error);
        diagnostics::write_service_trace(result.handle ? "TRACE semaphore creation accepted\n"
                                                       : "TRACE semaphore creation rejected\n");
        return reinterpret_cast<void *>(result.handle);
    } catch (const std::bad_alloc &) {
        set_last_error(8);
        return nullptr;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

void *AMBROSIA_GUEST_ABI create_semaphore_ex_a(void *attributes, std::int32_t initial, std::int32_t maximum,
                                               const char *name, std::uint32_t flags,
                                               std::uint32_t desired) noexcept {
    try {
        const auto wide =
            name ? std::optional(std::filesystem::path(guest::read_string(name)).u16string()) : std::nullopt;
        return create_semaphore_ex_w(attributes, initial, maximum, wide ? wide->c_str() : nullptr, flags,
                                     desired);
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

void *AMBROSIA_GUEST_ABI create_semaphore_w(void *attributes, std::int32_t initial, std::int32_t maximum,
                                            const char16_t *name) noexcept {
    return create_semaphore_ex_w(attributes, initial, maximum, name, 0, object_all_access);
}

void *AMBROSIA_GUEST_ABI create_semaphore_a(void *attributes, std::int32_t initial, std::int32_t maximum,
                                            const char *name) noexcept {
    return create_semaphore_ex_a(attributes, initial, maximum, name, 0, object_all_access);
}

std::int32_t AMBROSIA_GUEST_ABI release_semaphore(void *handle, std::int32_t amount,
                                                  std::int32_t *previous) noexcept {
    try {
        const auto result = objects().release_semaphore(reinterpret_cast<std::uintptr_t>(handle), amount);
        if (result.error) {
            set_last_error(result.error);
            return 0;
        }
        if (previous) {
            *previous = result.previous;
        }
        return 1;
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

std::int32_t AMBROSIA_GUEST_ABI close_handle(void *handle) noexcept {
    try {
        auto *runtime = current_runtime();
        if (runtime && runtime->registry_keys().owns(reinterpret_cast<std::uintptr_t>(handle))) {
            // CloseHandle reports success for registry handles but does not close
            // them. Their lifetime belongs to the distinct RegCloseKey operation.
            return 1;
        }
        return object_operation<&KernelObjects::close>(handle);
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
                               "api-ms-win-core-synch-l1-2-0.dll", "api-ms-win-core-synch-l1-2-1.dll"}) {
        registry.add(module, "CreateEventW", reinterpret_cast<std::uintptr_t>(&create_event_w));
        registry.add(module, "CreateEventA", reinterpret_cast<std::uintptr_t>(&create_event_a));
        registry.add(module, "CreateMutexA", reinterpret_cast<std::uintptr_t>(&create_mutex_a));
        registry.add(module, "CreateMutexW", reinterpret_cast<std::uintptr_t>(&create_mutex_w));
        registry.add(module, "ReleaseMutex",
                     reinterpret_cast<std::uintptr_t>(&object_operation<&KernelObjects::release_mutex>));
        registry.add(module, "SetEvent",
                     reinterpret_cast<std::uintptr_t>(&object_operation<&KernelObjects::set_event>));
        registry.add(module, "ResetEvent",
                     reinterpret_cast<std::uintptr_t>(&object_operation<&KernelObjects::reset_event>));
        registry.add(module, "WaitForSingleObject", reinterpret_cast<std::uintptr_t>(&wait_single));
        registry.add(module, "WaitForSingleObjectEx", reinterpret_cast<std::uintptr_t>(&wait_single_ex));
        registry.add(module, "CreateSemaphoreExW", reinterpret_cast<std::uintptr_t>(&create_semaphore_ex_w));
        registry.add(module, "CreateSemaphoreExA", reinterpret_cast<std::uintptr_t>(&create_semaphore_ex_a));
        registry.add(module, "CreateSemaphoreW", reinterpret_cast<std::uintptr_t>(&create_semaphore_w));
        registry.add(module, "CreateSemaphoreA", reinterpret_cast<std::uintptr_t>(&create_semaphore_a));
        registry.add(module, "ReleaseSemaphore", reinterpret_cast<std::uintptr_t>(&release_semaphore));
    }
    for (const auto *module : {"kernel32.dll", "kernelbase.dll", "api-ms-win-core-handle-l1-1-0.dll"}) {
        registry.add(module, "CloseHandle", reinterpret_cast<std::uintptr_t>(&close_handle));
    }
}
} // namespace ambrosia::services
