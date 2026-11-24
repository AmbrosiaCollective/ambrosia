#include "clock_services.h"
#include "service_runtime.h"
#include <cstring>

namespace ambrosia::services {
namespace {
void AMBROSIA_GUEST_ABI system_filetime(void *output) noexcept {
    if (!output) {
        set_last_error(error_invalid_parameter);
        return;
    }
    try {
        const auto time = host::utc_filetime();
        // FILETIME has two 32-bit fields and need not have 64-bit alignment.
        std::memcpy(output, &time, sizeof(time));
    } catch (...) {
        set_last_error(error_invalid_function);
    }
}

std::int32_t AMBROSIA_GUEST_ABI query_counter(void *output) noexcept {
    if (!output) {
        set_last_error(error_invalid_parameter);
        return 0;
    }
    const auto value = host::performance_counter();
    std::memcpy(output, &value, sizeof(value));
    return 1;
}

std::int32_t AMBROSIA_GUEST_ABI query_frequency(void *output) noexcept {
    if (!output) {
        set_last_error(error_invalid_parameter);
        return 0;
    }
    const auto value = host::performance_frequency;
    std::memcpy(output, &value, sizeof(value));
    return 1;
}

std::uint32_t AMBROSIA_GUEST_ABI process_id() noexcept {
    try {
        return host::current_process_id();
    } catch (...) {
        set_last_error(error_invalid_function);
        return 0;
    }
}

std::uint32_t AMBROSIA_GUEST_ABI thread_id() noexcept {
    try {
        return host::current_thread_id();
    } catch (...) {
        set_last_error(error_invalid_function);
        return 0;
    }
}
} // namespace

void register_clock_services(ImportRegistry &registry) {
    for (const auto *module : {"kernel32.dll", "kernelbase.dll", "api-ms-win-core-sysinfo-l1-1-0.dll"}) {
        registry.add(module, "GetSystemTimeAsFileTime", reinterpret_cast<std::uintptr_t>(&system_filetime));
    }
    for (const auto *module : {"kernel32.dll", "kernelbase.dll", "api-ms-win-core-profile-l1-1-0.dll"}) {
        registry.add(module, "QueryPerformanceCounter", reinterpret_cast<std::uintptr_t>(&query_counter));
        registry.add(module, "QueryPerformanceFrequency", reinterpret_cast<std::uintptr_t>(&query_frequency));
    }
    for (const auto *module :
         {"kernel32.dll", "kernelbase.dll", "api-ms-win-core-processthreads-l1-1-0.dll"}) {
        registry.add(module, "GetCurrentProcessId", reinterpret_cast<std::uintptr_t>(&process_id));
        registry.add(module, "GetCurrentThreadId", reinterpret_cast<std::uintptr_t>(&thread_id));
    }
}
} // namespace ambrosia::services
