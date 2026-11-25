#include "service_runtime.h"
#include "slim_locks.h"
#include "startup_trace.h"
#include <stdexcept>

namespace ambrosia::services {
namespace {
SlimLocks &locks() {
    auto *runtime = current_runtime();
    if (!runtime) {
        throw std::runtime_error("SRW locks require a scoped runtime");
    }
    return runtime->slim_locks();
}

void AMBROSIA_GUEST_ABI initialize(void *address) noexcept {
    try {
        locks().initialize(address);
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

template <bool Shared> void AMBROSIA_GUEST_ABI acquire(void *address) noexcept {
    try {
        (void)locks().acquire(address, Shared, false);
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

template <bool Shared> std::uint8_t AMBROSIA_GUEST_ABI try_acquire(void *address) noexcept {
    try {
        return locks().acquire(address, Shared, true) ? 1 : 0;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

template <bool Shared> void AMBROSIA_GUEST_ABI release(void *address) noexcept {
    try {
        locks().release(address, Shared);
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}
} // namespace

void register_slim_lock_services(ImportRegistry &registry) {
    for (const auto *module : {"kernel32.dll", "kernelbase.dll", "api-ms-win-core-synch-l1-1-0.dll",
                               "api-ms-win-core-synch-l1-2-0.dll", "api-ms-win-core-synch-l1-2-1.dll"}) {
        registry.add(module, "InitializeSRWLock", reinterpret_cast<std::uintptr_t>(&initialize));
        registry.add(module, "AcquireSRWLockExclusive", reinterpret_cast<std::uintptr_t>(&acquire<false>));
        registry.add(module, "AcquireSRWLockShared", reinterpret_cast<std::uintptr_t>(&acquire<true>));
        registry.add(module, "TryAcquireSRWLockExclusive",
                     reinterpret_cast<std::uintptr_t>(&try_acquire<false>));
        registry.add(module, "TryAcquireSRWLockShared", reinterpret_cast<std::uintptr_t>(&try_acquire<true>));
        registry.add(module, "ReleaseSRWLockExclusive", reinterpret_cast<std::uintptr_t>(&release<false>));
        registry.add(module, "ReleaseSRWLockShared", reinterpret_cast<std::uintptr_t>(&release<true>));
    }
}
} // namespace ambrosia::services
