#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "host.h"
#include <limits>
#include <stdexcept>
#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace ambrosia::host {
StackBounds current_stack_bounds() {
    StackBounds result;
#ifdef _WIN32
    ULONG_PTR lower = 0, upper = 0;
    GetCurrentThreadStackLimits(&lower, &upper);
    result = {lower, upper};
#elif defined(__linux__)
    pthread_attr_t attributes;
    if (pthread_getattr_np(pthread_self(), &attributes)) {
        throw std::runtime_error("cannot query native thread stack attributes");
    }
    void *base = nullptr;
    std::size_t bytes = 0;
    const auto status = pthread_attr_getstack(&attributes, &base, &bytes);
    const auto destroyed = pthread_attr_destroy(&attributes);
    const auto lower = reinterpret_cast<std::uintptr_t>(base);
    if (status || destroyed || !base || !bytes ||
        bytes > std::numeric_limits<std::uintptr_t>::max() - lower) {
        throw std::runtime_error("invalid native thread stack range");
    }
    result = {lower, lower + bytes};
#elif defined(__APPLE__)
    const auto upper = reinterpret_cast<std::uintptr_t>(pthread_get_stackaddr_np(pthread_self()));
    const auto bytes = pthread_get_stacksize_np(pthread_self());
    if (!bytes || upper < bytes) {
        throw std::runtime_error("invalid native thread stack range");
    }
    result = {upper - bytes, upper};
#else
    throw std::runtime_error("native stack bounds are unsupported on this host");
#endif
    const std::byte marker{};
    const auto address = reinterpret_cast<std::uintptr_t>(&marker);
    if (!result.lower || result.lower >= result.upper || address < result.lower || address >= result.upper) {
        throw std::runtime_error("native stack attributes do not contain the calling frame");
    }
    return result;
}
} // namespace ambrosia::host
