#include "host_priority.h"
#include <cerrno>
#include <limits>
#ifdef __linux__
#include <sched.h>
#include <sys/resource.h>
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
SchedulingError scheduling_error(int code) {
    if (code == EPERM || code == EACCES) {
        return SchedulingError::denied;
    }
    if (code == ESRCH) {
        return SchedulingError::missing_thread;
    }
    if (code == EINVAL) {
        return SchedulingError::invalid;
    }
    return SchedulingError::failure;
}
#endif
} // namespace

ThreadNiceness thread_niceness(std::uint32_t id) noexcept {
    NativeErrorScope restore;
#ifdef __linux__
    if (!id || id > static_cast<std::uint32_t>(std::numeric_limits<pid_t>::max())) {
        return {0, SchedulingError::invalid};
    }
    const auto policy = sched_getscheduler(static_cast<pid_t>(id));
    if (policy < 0) {
        return {0, scheduling_error(errno)};
    }
    if (policy != SCHED_OTHER) {
        return {0, SchedulingError::unsupported};
    }
    errno = 0;
    const auto value = getpriority(PRIO_PROCESS, static_cast<id_t>(id));
    if (errno) {
        return {0, scheduling_error(errno)};
    }
    return {value, SchedulingError::none};
#else
    (void)id;
    return {0, SchedulingError::unsupported};
#endif
}

SchedulingError set_thread_niceness(std::uint32_t id, int value) noexcept {
    NativeErrorScope restore;
    if (value < -20 || value > 19) {
        return SchedulingError::invalid;
    }
    const auto current = thread_niceness(id);
    if (current.error != SchedulingError::none) {
        return current.error;
    }
#ifdef __linux__
    if (setpriority(PRIO_PROCESS, static_cast<id_t>(id), value)) {
        return scheduling_error(errno);
    }
    const auto actual = thread_niceness(id);
    if (actual.error != SchedulingError::none) {
        return actual.error;
    }
    return actual.value == value ? SchedulingError::none : SchedulingError::failure;
#else
    return SchedulingError::unsupported;
#endif
}
} // namespace ambrosia::host
