#include "host.h"
#include <exception>
#include <stdexcept>
#if defined(__linux__) && defined(__x86_64__)
#include <asm/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace ambrosia::host {
std::uintptr_t guest_segment_base() {
#if defined(__linux__) && defined(__x86_64__)
    unsigned long value = 0;
    if (syscall(SYS_arch_prctl, ARCH_GET_GS, &value)) {
        throw std::runtime_error("cannot read guest GS base");
    }
    return value;
#else
    throw std::runtime_error("guest thread adapter is not implemented for this host");
#endif
}

namespace {
bool set_guest_segment(std::uintptr_t value) noexcept {
#if defined(__linux__) && defined(__x86_64__)
    return syscall(SYS_arch_prctl, ARCH_SET_GS, value) == 0;
#else
    (void)value;
    return false;
#endif
}
} // namespace

GuestSegmentScope::GuestSegmentScope(void *base) : saved_(guest_segment_base()) {
    if (!base || !set_guest_segment(reinterpret_cast<std::uintptr_t>(base))) {
        throw std::runtime_error("cannot install guest GS base");
    }
}

GuestSegmentScope::~GuestSegmentScope() {
    // Continuing with a dangling guest segment would corrupt the host thread.
    if (!set_guest_segment(saved_)) {
        std::terminate();
    }
}
} // namespace ambrosia::host
