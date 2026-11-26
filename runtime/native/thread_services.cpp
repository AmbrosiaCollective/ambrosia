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

std::uintptr_t AMBROSIA_GUEST_ABI create_thread(void *attributes, std::size_t stack_bytes, ThreadEntry entry,
                                                void *argument, std::uint32_t flags,
                                                void *id_output) noexcept {
    RestoreNativeError restore;
    if (attributes || flags) {
        diagnostics::stop_trace("thread security attributes/suspended/reservation flags are not modeled", 78);
    }
    try {
        auto *graph = current_modules();
        if (!graph) {
            diagnostics::stop_trace("thread creation requires a live module graph", 78);
        }
        auto &owner = runtime();
        const auto result = owner.threads().create(owner, *graph, entry, argument, stack_bytes);
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

std::uint32_t AMBROSIA_GUEST_ABI thread_id(std::uintptr_t handle) noexcept {
    RestoreNativeError restore;
    try {
        const auto id = runtime().threads().thread_id(handle);
        if (!id) {
            set_last_error(6);
        }
        return id;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}
} // namespace

void register_thread_services(ImportRegistry &registry) {
    for (const auto *module : {"kernel32.dll", "kernelbase.dll", "api-ms-win-core-processthreads-l1-1-0.dll",
                               "api-ms-win-core-processthreads-l1-1-1.dll"}) {
        registry.add(module, "CreateThread", reinterpret_cast<std::uintptr_t>(&create_thread));
        registry.add(module, "GetExitCodeThread", reinterpret_cast<std::uintptr_t>(&exit_code));
        registry.add(module, "GetThreadId", reinterpret_cast<std::uintptr_t>(&thread_id));
    }
}
} // namespace ambrosia::services
