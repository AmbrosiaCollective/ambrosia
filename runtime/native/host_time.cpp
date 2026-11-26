#include "host.h"
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <time.h>
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

std::uint64_t precise_utc_filetime() {
#ifdef _WIN32
    FILETIME value{};
    GetSystemTimePreciseAsFileTime(&value);
    return static_cast<std::uint64_t>(value.dwLowDateTime) |
           (static_cast<std::uint64_t>(value.dwHighDateTime) << 32);
#elif defined(__linux__)
    static_assert(std::chrono::system_clock::period::num * 1000000 <= std::chrono::system_clock::period::den);
    timespec resolution{};
    if (clock_getres(CLOCK_REALTIME, &resolution) || resolution.tv_sec != 0 || resolution.tv_nsec <= 0 ||
        resolution.tv_nsec > 1000) {
        throw std::runtime_error("native UTC clock lacks microsecond resolution");
    }
    // Reuse the checked epoch conversion and native system_clock source.
    // Clock resolution does not promise synchronization or absolute accuracy.
    return utc_filetime();
#else
    throw std::runtime_error("native precise UTC adapter is unsupported");
#endif
}

void sleep_milliseconds(std::uint32_t milliseconds) {
    if (!milliseconds) {
        std::this_thread::yield();
        return;
    }
    if (milliseconds == 0xffffffffu) {
        // An infinite, non-alertable guest sleep never returns on a spurious
        // host wake. Process teardown remains responsible for termination.
        std::mutex mutex;
        std::condition_variable changed;
        std::unique_lock lock(mutex);
        changed.wait(lock, [] { return false; });
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

std::uint64_t uptime_from_parts(std::int64_t seconds, std::int64_t nanoseconds) {
    if (seconds < 0 || nanoseconds < 0 || nanoseconds >= 1000000000) {
        throw std::range_error("invalid system uptime clock value");
    }
    const auto fraction = static_cast<std::uint64_t>(nanoseconds) / 1000000;
    const auto whole = static_cast<std::uint64_t>(seconds);
    if (whole > (std::numeric_limits<std::uint64_t>::max() - fraction) / 1000) {
        throw std::range_error("system uptime exceeds millisecond representation");
    }
    return whole * 1000 + fraction;
}

std::uint64_t uptime_milliseconds() {
#ifdef _WIN32
    return GetTickCount64();
#elif defined(__linux__)
    timespec value{};
    // Unlike steady_clock/CLOCK_MONOTONIC, BOOTTIME includes suspend. Guest
    // uptime is system-relative, never an offset from runtime construction.
    if (clock_gettime(CLOCK_BOOTTIME, &value)) {
        throw std::runtime_error("native boot-time clock is unavailable");
    }
    return uptime_from_parts(value.tv_sec, value.tv_nsec);
#else
    throw std::runtime_error("native system-uptime adapter is unsupported on this host");
#endif
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
