#include "crt_startup.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include <cstring>

namespace ambrosia::services {
namespace {
CrtStartup &startup() noexcept {
    auto *runtime = current_runtime();
    if (!runtime) {
        diagnostics::stop_trace("CRT startup state requires a scoped runtime", 78);
    }
    return runtime->startup();
}

void AMBROSIA_GUEST_ABI set_app_type(std::int32_t type) noexcept {
    if (!startup().set_app_type(type)) {
        diagnostics::stop_trace("invalid CRT application type", 78);
    }
}

std::int32_t AMBROSIA_GUEST_ABI set_file_mode(std::int32_t mode) noexcept {
    const auto error = startup().set_file_mode(mode);
    if (error) {
        crt_error() = error;
    }
    return error;
}

std::int32_t AMBROSIA_GUEST_ABI get_file_mode(void *output) noexcept {
    if (!output) {
        crt_error() = 22;
        return 22;
    }
    const auto mode = *startup().file_mode();
    std::memcpy(output, &mode, sizeof(mode));
    return 0;
}

std::int32_t *AMBROSIA_GUEST_ABI file_mode() noexcept {
    return startup().file_mode();
}

std::uint32_t *AMBROSIA_GUEST_ABI commit_mode() noexcept {
    return startup().commit_mode();
}

std::int32_t AMBROSIA_GUEST_ABI configure_arguments(std::int32_t mode) noexcept {
    const auto error = startup().configure_arguments(mode);
    if (error) {
        crt_error() = error;
    }
    return error;
}

std::int32_t *AMBROSIA_GUEST_ABI argument_count() noexcept {
    return startup().argument_count();
}

char ***AMBROSIA_GUEST_ABI argument_vector() noexcept {
    return startup().argument_vector();
}

char **AMBROSIA_GUEST_ABI program_name() noexcept {
    return startup().program_name();
}

char *AMBROSIA_GUEST_ABI winmain_arguments() noexcept {
    return startup().winmain_arguments();
}

std::int32_t AMBROSIA_GUEST_ABI initialize_environment() noexcept {
    return startup().initialize_environment();
}

char ***AMBROSIA_GUEST_ABI environment() noexcept {
    return startup().environment();
}

char **AMBROSIA_GUEST_ABI initial_environment() noexcept {
    return startup().initial_environment();
}
} // namespace

void register_crt_startup(ImportRegistry &registry) {
    for (const auto *module : {"ucrtbase.dll", "msvcrt.dll", "api-ms-win-crt-runtime-l1-1-0.dll"}) {
        registry.add(module, "_set_app_type", reinterpret_cast<std::uintptr_t>(&set_app_type));
        registry.add(module, "__set_app_type", reinterpret_cast<std::uintptr_t>(&set_app_type));
        registry.add(module, "_configure_narrow_argv",
                     reinterpret_cast<std::uintptr_t>(&configure_arguments));
        registry.add(module, "__p___argc", reinterpret_cast<std::uintptr_t>(&argument_count));
        registry.add(module, "__p___argv", reinterpret_cast<std::uintptr_t>(&argument_vector));
        registry.add(module, "__p__pgmptr", reinterpret_cast<std::uintptr_t>(&program_name));
        registry.add(module, "_get_narrow_winmain_command_line",
                     reinterpret_cast<std::uintptr_t>(&winmain_arguments));
        registry.add(module, "_initialize_narrow_environment",
                     reinterpret_cast<std::uintptr_t>(&initialize_environment));
        registry.add(module, "_get_initial_narrow_environment",
                     reinterpret_cast<std::uintptr_t>(&initial_environment));
    }
    for (const auto *module : {"ucrtbase.dll", "msvcrt.dll", "api-ms-win-crt-stdio-l1-1-0.dll"}) {
        registry.add(module, "_set_fmode", reinterpret_cast<std::uintptr_t>(&set_file_mode));
        registry.add(module, "_get_fmode", reinterpret_cast<std::uintptr_t>(&get_file_mode));
        registry.add(module, "__p__fmode", reinterpret_cast<std::uintptr_t>(&file_mode));
        registry.add(module, "__p__commode", reinterpret_cast<std::uintptr_t>(&commit_mode));
    }
    for (const auto *module : {"ucrtbase.dll", "msvcrt.dll", "api-ms-win-crt-environment-l1-1-0.dll"}) {
        registry.add(module, "__p__environ", reinterpret_cast<std::uintptr_t>(&environment));
    }
}
} // namespace ambrosia::services
