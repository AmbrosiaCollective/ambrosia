#include "host.h"
#include <chrono>
#include <stdexcept>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#else
#include <unistd.h>
#endif

namespace ambrosia::host {
std::uint64_t filetime_from_unix_ticks(std::int64_t ticks_100ns) {
    // FILETIME counts 100 ns since 1601-01-01; system_clock counts from 1970.
    constexpr std::int64_t epoch = 116444736000000000;
    if (ticks_100ns < -epoch) {
        throw std::range_error("time precedes the FILETIME epoch");
    }
    if (ticks_100ns < 0) {
        return static_cast<std::uint64_t>(epoch - (-ticks_100ns));
    }
    return static_cast<std::uint64_t>(epoch) + static_cast<std::uint64_t>(ticks_100ns);
}

std::uint64_t utc_filetime() {
    using FileTicks = std::chrono::duration<std::int64_t, std::ratio<1, 10000000>>;
    const auto elapsed = std::chrono::system_clock::now().time_since_epoch();
    return filetime_from_unix_ticks(std::chrono::duration_cast<FileTicks>(elapsed).count());
}

std::int64_t performance_counter() noexcept {
    const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
}

std::uint32_t current_process_id() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    const auto process = getpid();
    if (process <= 0) {
        throw std::runtime_error("native process ID is unavailable");
    }
    return static_cast<std::uint32_t>(process);
#endif
}

std::uint32_t current_thread_id() {
#ifdef _WIN32
    return GetCurrentThreadId();
#elif defined(__linux__)
    const auto thread = syscall(SYS_gettid);
    if (thread <= 0) {
        throw std::runtime_error("native thread ID is unavailable");
    }
    return static_cast<std::uint32_t>(thread);
#else
    throw std::runtime_error("native thread ID adapter is unsupported");
#endif
}
} // namespace ambrosia::host
