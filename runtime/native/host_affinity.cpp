#include "host_affinity.h"
#include <algorithm>
#include <cerrno>
#include <limits>
#include <stdexcept>
#ifdef __linux__
#include <sched.h>
#include <sys/types.h>
#endif

namespace ambrosia::host {
namespace {
struct NativeErrorScope {
    int saved = errno;

    ~NativeErrorScope() {
        errno = saved;
    }
};
#ifdef __linux__
SchedulingError affinity_error(int code) {
    if (code == EPERM || code == EACCES) {
        return SchedulingError::denied;
    }
    if (code == ESRCH) {
        return SchedulingError::missing_thread;
    }
    return code == EINVAL ? SchedulingError::invalid : SchedulingError::failure;
}
#endif
} // namespace

ThreadProcessors thread_processors(std::uint32_t id) {
    NativeErrorScope restore;
#ifdef __linux__
    if (!id || id > static_cast<std::uint32_t>(std::numeric_limits<pid_t>::max())) {
        return {{}, SchedulingError::invalid};
    }
    cpu_set_t mask;
    CPU_ZERO(&mask);
    if (sched_getaffinity(static_cast<pid_t>(id), sizeof(mask), &mask)) {
        return {{}, affinity_error(errno)};
    }
    ThreadProcessors result;
    for (unsigned processor = 0; processor < CPU_SETSIZE; ++processor) {
        if (CPU_ISSET(processor, &mask)) {
            result.ids.push_back(processor);
        }
    }
    if (result.ids.empty()) {
        result.error = SchedulingError::failure;
    }
    return result;
#else
    (void)id;
    return {{}, SchedulingError::unsupported};
#endif
}

SchedulingError set_thread_processors(std::uint32_t id, std::span<const unsigned> processors) {
    NativeErrorScope restore;
    if (processors.empty() || processors.size() > 64 ||
        !std::is_sorted(processors.begin(), processors.end()) ||
        std::adjacent_find(processors.begin(), processors.end()) != processors.end()) {
        return SchedulingError::invalid;
    }
    const auto before = thread_processors(id);
    if (before.error != SchedulingError::none) {
        return before.error;
    }
#ifdef __linux__
    cpu_set_t requested;
    CPU_ZERO(&requested);
    for (const auto processor : processors) {
        if (processor >= CPU_SETSIZE) {
            return SchedulingError::invalid;
        }
        CPU_SET(processor, &requested);
    }
    cpu_set_t prior;
    CPU_ZERO(&prior);
    for (const auto processor : before.ids) {
        CPU_SET(processor, &prior);
    }
    // All allocation/validation precedes mutation. Verification and rollback
    // use stack masks so an allocation failure cannot leave an unreported change.
    if (sched_setaffinity(static_cast<pid_t>(id), sizeof(requested), &requested)) {
        return affinity_error(errno);
    }
    cpu_set_t actual;
    CPU_ZERO(&actual);
    if (!sched_getaffinity(static_cast<pid_t>(id), sizeof(actual), &actual) &&
        CPU_EQUAL(&actual, &requested)) {
        return SchedulingError::none;
    }
    if (sched_setaffinity(static_cast<pid_t>(id), sizeof(prior), &prior)) {
        throw std::runtime_error("native thread affinity verification and rollback failed");
    }
    CPU_ZERO(&actual);
    if (sched_getaffinity(static_cast<pid_t>(id), sizeof(actual), &actual) || !CPU_EQUAL(&actual, &prior)) {
        throw std::runtime_error("native thread affinity rollback could not be verified");
    }
    return SchedulingError::failure;
#else
    return SchedulingError::unsupported;
#endif
}
} // namespace ambrosia::host
