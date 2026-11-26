#include "invalid_parameters.h"
#include "guest_thread.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include <cerrno>
#include <stdexcept>

namespace ambrosia::services {
namespace {
InvalidParameters &parameters() {
    auto *runtime = current_runtime();
    if (!runtime) {
        diagnostics::stop_trace("CRT invalid-parameter hooks require a scoped runtime", 78);
    }
    return runtime->invalid_parameters();
}

guest::ThreadContext &thread() {
    auto *context = guest::ThreadContext::current();
    if (!context) {
        diagnostics::stop_trace("CRT local invalid-parameter hooks require active guest TLS", 78);
    }
    return *context;
}

InvalidParameterHandler AMBROSIA_GUEST_ABI set_global(InvalidParameterHandler handler) noexcept {
    return parameters().replace(handler);
}

InvalidParameterHandler AMBROSIA_GUEST_ABI get_global() noexcept {
    return parameters().handler();
}

InvalidParameterHandler AMBROSIA_GUEST_ABI set_local(InvalidParameterHandler handler) noexcept {
    (void)parameters();
    return reinterpret_cast<InvalidParameterHandler>(
        thread().replace_invalid_parameter_handler(reinterpret_cast<std::uintptr_t>(handler)));
}

InvalidParameterHandler AMBROSIA_GUEST_ABI get_local() noexcept {
    (void)parameters();
    return reinterpret_cast<InvalidParameterHandler>(thread().invalid_parameter_handler());
}

void AMBROSIA_GUEST_ABI invalid_parameter(const char16_t *expression, const char16_t *function,
                                          const char16_t *file, std::uint32_t line,
                                          std::uintptr_t reserved) noexcept {
    dispatch_invalid_parameter(expression, function, file, line, reserved);
}

void AMBROSIA_GUEST_ABI no_information() noexcept {
    dispatch_invalid_parameter();
}

[[noreturn]] void AMBROSIA_GUEST_ABI no_information_fatal() noexcept {
    dispatch_invalid_parameter();
    diagnostics::stop_trace("CRT nonreturning invalid-parameter Watson termination is not modeled", 78);
}
} // namespace

void dispatch_invalid_parameter(const char16_t *expression, const char16_t *function, const char16_t *file,
                                std::uint32_t line, std::uintptr_t reserved) noexcept {
    const auto saved_error = errno;
    auto &global = parameters();
    auto handler = reinterpret_cast<InvalidParameterHandler>(thread().invalid_parameter_handler());
    if (!handler) {
        handler = global.handler();
    }
    if (!handler) {
        diagnostics::stop_trace("CRT default invalid-parameter Watson termination is not modeled", 78);
    }
    try {
        handler(expression, function, file, line, reserved);
    } catch (...) {
        diagnostics::stop_trace("guest invalid-parameter exception propagation is not implemented", 78);
    }
    errno = saved_error;
}

void register_invalid_parameters(ImportRegistry &registry) {
    for (const auto *module : {"ucrtbase.dll", "api-ms-win-crt-runtime-l1-1-0.dll"}) {
        registry.add(module, "_set_invalid_parameter_handler", reinterpret_cast<std::uintptr_t>(&set_global));
        registry.add(module, "_get_invalid_parameter_handler", reinterpret_cast<std::uintptr_t>(&get_global));
        registry.add(module, "_set_thread_local_invalid_parameter_handler",
                     reinterpret_cast<std::uintptr_t>(&set_local));
        registry.add(module, "_get_thread_local_invalid_parameter_handler",
                     reinterpret_cast<std::uintptr_t>(&get_local));
        registry.add(module, "_invalid_parameter", reinterpret_cast<std::uintptr_t>(&invalid_parameter));
        registry.add(module, "_invalid_parameter_noinfo", reinterpret_cast<std::uintptr_t>(&no_information));
        registry.add(module, "_invalid_parameter_noinfo_noreturn",
                     reinterpret_cast<std::uintptr_t>(&no_information_fatal));
    }
}
} // namespace ambrosia::services
