#include "clock_services.h"
#include "service_runtime.h"
#include <cerrno>
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

std::int64_t AMBROSIA_GUEST_ABI current_crt_time(void *output) noexcept {
    const auto saved_error = errno;
    std::int64_t seconds = -1;
    try {
        seconds = crt_seconds_from_filetime(host::utc_filetime());
    } catch (...) {
        // A host clock failure is a failed CRT query, never a successful epoch.
    }
    if (seconds < 0) {
        crt_error() = 22;
    }
    if (output) {
        std::memcpy(output, &seconds, sizeof(seconds));
    }
    errno = saved_error;
    return seconds;
}

double AMBROSIA_GUEST_ABI time_difference(std::int64_t end, std::int64_t beginning) noexcept {
    // Valid CRT times fit exactly in double. Subtract after conversion so even
    // out-of-contract extreme inputs cannot overflow a native signed integer.
    return static_cast<double>(end) - static_cast<double>(beginning);
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

std::int64_t crt_seconds_from_filetime(std::uint64_t filetime) noexcept {
    constexpr std::uint64_t epoch = 116444736000000000;
    if (filetime < epoch) {
        return -1;
    }
    const auto seconds = (filetime - epoch) / 10000000;
    // Even UINT64_MAX FILETIME ticks become fewer than 2^41 seconds, so the
    // subtraction/division result fits int64 without a calendar/timezone cap.
    return static_cast<std::int64_t>(seconds);
}

void register_clock_services(ImportRegistry &registry) {
    for (const auto *module : {"ucrtbase.dll", "msvcrt.dll", "api-ms-win-crt-time-l1-1-0.dll"}) {
        registry.add(module, "_time64", reinterpret_cast<std::uintptr_t>(&current_crt_time));
        registry.add(module, "_difftime64", reinterpret_cast<std::uintptr_t>(&time_difference));
    }
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
