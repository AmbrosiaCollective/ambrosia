#include "native_threads.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace ambrosia::services {
namespace {
struct RestoreNativeError {
    int saved = errno;

    ~RestoreNativeError() {
        errno = saved;
    }
};

Runtime &runtime() {
    auto *value = current_runtime();
    if (!value) {
        diagnostics::stop_trace("thread services require a scoped runtime", 78);
    }
    return *value;
}

std::uintptr_t AMBROSIA_GUEST_ABI current_process() noexcept {
    // Guest ABI pseudo-handle, never a native descriptor or an owned object.
    return static_cast<std::uintptr_t>(-1);
}

std::uintptr_t AMBROSIA_GUEST_ABI create_thread(void *attributes, std::size_t stack_bytes, ThreadEntry entry,
                                                void *argument, std::uint32_t flags,
                                                void *id_output) noexcept {
    RestoreNativeError restore;
    if (attributes || (flags & ~std::uint32_t{4})) {
        diagnostics::stop_trace("thread security attributes/reservation/unknown flags are not modeled", 78);
    }
    try {
        auto *graph = current_modules();
        if (!graph) {
            diagnostics::stop_trace("thread creation requires a live module graph", 78);
        }
        auto &owner = runtime();
        const auto result = owner.threads().create(owner, *graph, entry, argument, stack_bytes, flags & 4);
        if (result.error) {
            set_last_error(result.error);
            return 0;
        }
        if (id_output) {
            std::memcpy(id_output, &result.id, sizeof(result.id));
        }
        diagnostics::write_service_trace("TRACE native guest thread created\n");
        return result.handle;
    } catch (const std::bad_alloc &) {
        set_last_error(8);
        return 0;
    } catch (const std::system_error &) {
        set_last_error(8);
        return 0;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

std::uintptr_t AMBROSIA_GUEST_ABI open_thread(std::uint32_t access, std::int32_t inherited,
                                              std::uint32_t id) noexcept {
    RestoreNativeError restore;
    if (access & ~thread_all_access) {
        diagnostics::stop_trace("generic/maximum/security thread access masks are not modeled", 78);
    }
    try {
        const auto result = runtime().threads().open(id, access, inherited != 0);
        if (result.error) {
            set_last_error(result.error);
            return 0;
        }
        return result.handle;
    } catch (const std::bad_alloc &) {
        set_last_error(8);
        return 0;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

std::int32_t AMBROSIA_GUEST_ABI exit_code(std::uintptr_t handle, void *output) noexcept {
    RestoreNativeError restore;
    try {
        const auto error = runtime().threads().exit_code(handle, output);
        if (error) {
            set_last_error(error);
            return 0;
        }
        return 1;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

std::int32_t AMBROSIA_GUEST_ABI set_priority(std::uintptr_t handle, std::int32_t priority) noexcept {
    RestoreNativeError restore;
    try {
        const auto error = runtime().threads().set_priority(handle, priority);
        if (error) {
            set_last_error(error);
            diagnostics::write_service_trace("TRACE native thread priority request rejected by adapter\n");
            return 0;
        }
        return 1;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

std::int32_t AMBROSIA_GUEST_ABI get_priority(std::uintptr_t handle) noexcept {
    RestoreNativeError restore;
    try {
        const auto result = runtime().threads().priority(handle);
        if (result.error) {
            set_last_error(result.error);
        }
        return result.value;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

std::int32_t AMBROSIA_GUEST_ABI set_process_boost(std::uintptr_t process, std::int32_t disabled) noexcept {
    RestoreNativeError restore;
    (void)disabled;
    if (process != static_cast<std::uintptr_t>(-1)) {
        set_last_error(6);
        return 0;
    }
    // The current guest process is recognized, but no native-host backend
    // implements Windows dynamic boosts. Let callers handle this real failure.
    set_last_error(50);
    diagnostics::write_service_trace("TRACE native process priority boost unavailable\n");
    return 0;
}

std::int32_t AMBROSIA_GUEST_ABI set_thread_boost(std::uintptr_t handle, std::int32_t disabled) noexcept {
    RestoreNativeError restore;
    try {
        const auto error = runtime().threads().set_priority_boost(handle, disabled != 0);
        if (error) {
            set_last_error(error);
            return 0;
        }
        return 1;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

std::uint32_t AMBROSIA_GUEST_ABI resume_thread(std::uintptr_t handle) noexcept {
    RestoreNativeError restore;
    try {
        const auto result = runtime().threads().resume(handle);
        if (result.error) {
            set_last_error(result.error);
        }
        return result.previous;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

std::uint32_t AMBROSIA_GUEST_ABI thread_id(std::uintptr_t handle) noexcept {
    RestoreNativeError restore;
    try {
        const auto result = runtime().threads().identify(handle);
        if (result.error) {
            set_last_error(result.error);
        }
        return result.id;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}
} // namespace

void register_thread_services(ImportRegistry &registry) {
    for (const auto *module :
         {"kernel32.dll", "kernelbase.dll", "api-ms-win-core-processthreads-l1-1-0.dll",
          "api-ms-win-core-processthreads-l1-1-1.dll", "api-ms-win-core-processthreads-l1-1-2.dll"}) {
        registry.add(module, "SetProcessPriorityBoost", reinterpret_cast<std::uintptr_t>(&set_process_boost));
        registry.add(module, "SetThreadPriorityBoost", reinterpret_cast<std::uintptr_t>(&set_thread_boost));
        registry.add(module, "GetCurrentProcess", reinterpret_cast<std::uintptr_t>(&current_process));
        registry.add(module, "CreateThread", reinterpret_cast<std::uintptr_t>(&create_thread));
        registry.add(module, "OpenThread", reinterpret_cast<std::uintptr_t>(&open_thread));
        registry.add(module, "GetExitCodeThread", reinterpret_cast<std::uintptr_t>(&exit_code));
        registry.add(module, "GetThreadId", reinterpret_cast<std::uintptr_t>(&thread_id));
        registry.add(module, "ResumeThread", reinterpret_cast<std::uintptr_t>(&resume_thread));
        registry.add(module, "SetThreadPriority", reinterpret_cast<std::uintptr_t>(&set_priority));
        registry.add(module, "GetThreadPriority", reinterpret_cast<std::uintptr_t>(&get_priority));
    }
}
} // namespace ambrosia::services
