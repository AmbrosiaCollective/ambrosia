#include "service_runtime.h"
#include "startup_trace.h"
#include "thread_affinity.h"
#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace ambrosia::services {
namespace {
struct NativeErrorScope {
    int saved = errno;

    ~NativeErrorScope() {
        errno = saved;
    }
};

Runtime &runtime() {
    auto *owner = current_runtime();
    if (!owner) {
        diagnostics::stop_trace("thread affinity requires a scoped runtime", 78);
    }
    return *owner;
}

std::uint32_t read_affinity(std::uintptr_t handle, GroupAffinity &output) {
    auto &owner = runtime();
    const ThreadAffinity mapping(owner.processor_information().processor_ids());
    return owner.threads().group_affinity(handle, mapping, output);
}

std::uint32_t write_affinity(std::uintptr_t handle, const GroupAffinity &value, GroupAffinity &previous) {
    auto &owner = runtime();
    const ThreadAffinity mapping(owner.processor_information().processor_ids());
    return owner.threads().set_group_affinity(handle, mapping, value, previous);
}

std::int32_t AMBROSIA_GUEST_ABI get_group(std::uintptr_t handle, void *output) noexcept {
    NativeErrorScope restore;
    if (!output) {
        set_last_error(87);
        return 0;
    }
    try {
        GroupAffinity result;
        const auto error = read_affinity(handle, result);
        if (error) {
            set_last_error(error);
            return 0;
        }
        std::memcpy(output, &result, sizeof(result));
        return 1;
    } catch (const std::bad_alloc &) {
        set_last_error(8);
        return 0;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

std::int32_t AMBROSIA_GUEST_ABI set_group(std::uintptr_t handle, const void *value, void *previous) noexcept {
    NativeErrorScope restore;
    if (!value) {
        set_last_error(87);
        return 0;
    }
    try {
        GroupAffinity requested;
        std::memcpy(&requested, value, sizeof(requested));
        GroupAffinity before;
        const auto error = write_affinity(handle, requested, before);
        if (error) {
            set_last_error(error);
            return 0;
        }
        if (previous) {
            std::memcpy(previous, &before, sizeof(before));
        }
        return 1;
    } catch (const std::bad_alloc &) {
        set_last_error(8);
        return 0;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

std::uint64_t AMBROSIA_GUEST_ABI set_mask(std::uintptr_t handle, std::uint64_t mask) noexcept {
    NativeErrorScope restore;
    try {
        const GroupAffinity requested{mask, 0, {}};
        GroupAffinity previous;
        const auto error = write_affinity(handle, requested, previous);
        if (error) {
            set_last_error(error);
            return 0;
        }
        return previous.mask;
    } catch (const std::bad_alloc &) {
        set_last_error(8);
        return 0;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}
} // namespace

void register_affinity_services(ImportRegistry &registry) {
    for (const auto *module : {"kernel32.dll", "kernelbase.dll", "api-ms-win-core-processtopology-l1-1-0.dll",
                               "api-ms-win-core-processtopology-l1-1-1.dll"}) {
        registry.add(module, "GetThreadGroupAffinity", reinterpret_cast<std::uintptr_t>(&get_group));
        registry.add(module, "SetThreadGroupAffinity", reinterpret_cast<std::uintptr_t>(&set_group));
        registry.add(module, "SetThreadAffinityMask", reinterpret_cast<std::uintptr_t>(&set_mask));
    }
}
} // namespace ambrosia::services
