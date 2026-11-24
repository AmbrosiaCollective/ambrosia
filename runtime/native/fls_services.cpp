#include "fls_services.h"
#include "fls_slots.h"
#include "service_runtime.h"
#include "startup_trace.h"

namespace ambrosia::services {
namespace {
std::uint32_t AMBROSIA_GUEST_ABI allocate(FlsCallback callback) noexcept {
    auto *runtime = current_runtime();
    if (!runtime) {
        set_last_error(error_invalid_function);
        return fls_out_of_indexes;
    }
    const auto result = runtime->fls().allocate(callback);
    if (result.error) {
        set_last_error(result.error);
    }
    return result.index;
}

void *AMBROSIA_GUEST_ABI get(std::uint32_t index) noexcept {
    auto *runtime = current_runtime();
    auto *context = guest::ThreadContext::current();
    if (!runtime || !context) {
        set_last_error(error_invalid_function);
        return nullptr;
    }
    const auto result = runtime->fls().get(index, *context);
    // FlsGetValue clears last error on success, including a stored NULL value.
    set_last_error(result.error);
    return result.value;
}

std::int32_t AMBROSIA_GUEST_ABI set(std::uint32_t index, void *value) noexcept {
    auto *runtime = current_runtime();
    auto *context = guest::ThreadContext::current();
    if (!runtime || !context) {
        set_last_error(error_invalid_function);
        return 0;
    }
    const auto error = runtime->fls().set(index, value, *context);
    if (error) {
        set_last_error(error);
    }
    return error ? 0 : 1;
}

std::int32_t AMBROSIA_GUEST_ABI free(std::uint32_t index) noexcept {
    auto *runtime = current_runtime();
    if (!runtime) {
        set_last_error(error_invalid_function);
        return 0;
    }
    try {
        const auto error = runtime->fls().free(index);
        if (error) {
            set_last_error(error);
        }
        return error ? 0 : 1;
    } catch (const std::bad_alloc &) {
        set_last_error(error_not_enough_memory);
        return 0;
    } catch (...) {
        diagnostics::stop_trace("FLS cleanup callback failed", 78);
    }
}
} // namespace

void register_fls_services(ImportRegistry &registry) {
    for (const auto *module : {"kernel32.dll", "kernelbase.dll", "api-ms-win-core-fibers-l1-1-0.dll"}) {
        registry.add(module, "FlsAlloc", reinterpret_cast<std::uintptr_t>(&allocate));
        registry.add(module, "FlsGetValue", reinterpret_cast<std::uintptr_t>(&get));
        registry.add(module, "FlsSetValue", reinterpret_cast<std::uintptr_t>(&set));
        registry.add(module, "FlsFree", reinterpret_cast<std::uintptr_t>(&free));
    }
}
} // namespace ambrosia::services
