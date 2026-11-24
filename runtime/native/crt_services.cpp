#include "crt_lifecycle.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include <cstring>

namespace ambrosia::services {
namespace {
std::int32_t AMBROSIA_GUEST_ABI initialize_table(void *table) noexcept {
    auto *runtime = current_runtime();
    return runtime ? runtime->on_exit().initialize(table) : -1;
}

std::int32_t AMBROSIA_GUEST_ABI register_callback(void *table, OnExitCallback callback) noexcept {
    auto *runtime = current_runtime();
    return runtime ? runtime->on_exit().add(table, callback) : -1;
}

std::int32_t AMBROSIA_GUEST_ABI execute_table(void *table) noexcept {
    auto *runtime = current_runtime();
    if (!runtime) {
        return -1;
    }
    try {
        return runtime->on_exit().execute(table);
    } catch (...) {
        diagnostics::stop_trace("CRT on-exit callback failed", 78);
    }
}

std::size_t initializer_count(const void *begin, const void *end) {
    const auto first = reinterpret_cast<std::uintptr_t>(begin);
    const auto last = reinterpret_cast<std::uintptr_t>(end);
    if (last < first || (last - first) % 8 || (last - first) / 8 > 4096 || (!begin && last != first)) {
        diagnostics::stop_trace("invalid CRT initializer range", 78);
    }
    return (last - first) / 8;
}

void AMBROSIA_GUEST_ABI initialize_void(const void *begin, const void *end) noexcept {
    const auto count = initializer_count(begin, end);
    const auto *entries = static_cast<const std::byte *>(begin);
    for (std::size_t index = 0; index < count; ++index) {
        Initializer callback = nullptr;
        std::memcpy(&callback, entries + index * 8, 8);
        if (callback) {
            callback();
        }
    }
}

std::int32_t AMBROSIA_GUEST_ABI initialize_error(const void *begin, const void *end) noexcept {
    const auto count = initializer_count(begin, end);
    const auto *entries = static_cast<const std::byte *>(begin);
    for (std::size_t index = 0; index < count; ++index) {
        OnExitCallback callback = nullptr;
        std::memcpy(&callback, entries + index * 8, 8);
        if (callback) {
            const auto result = callback();
            if (result) {
                return result;
            }
        }
    }
    return 0;
}
} // namespace

void register_crt_lifecycle(ImportRegistry &registry) {
    for (const auto *module : {"ucrtbase.dll", "api-ms-win-crt-runtime-l1-1-0.dll", "msvcrt.dll"}) {
        registry.add(module, "_initialize_onexit_table", reinterpret_cast<std::uintptr_t>(&initialize_table));
        registry.add(module, "_register_onexit_function",
                     reinterpret_cast<std::uintptr_t>(&register_callback));
        registry.add(module, "_execute_onexit_table", reinterpret_cast<std::uintptr_t>(&execute_table));
        registry.add(module, "_initterm", reinterpret_cast<std::uintptr_t>(&initialize_void));
        registry.add(module, "_initterm_e", reinterpret_cast<std::uintptr_t>(&initialize_error));
    }
}
} // namespace ambrosia::services
