#pragma once
#include "kernel_objects.h"
#include "module_graph.h"
#include "thread_affinity.h"
#include <optional>
#include <thread>
#include <vector>

namespace ambrosia::services {
class Runtime;
class ImportRegistry;
using ThreadEntry = std::uint32_t(AMBROSIA_GUEST_ABI *)(void *);
inline constexpr std::uint32_t thread_query_access = 0x40 | 0x800;
inline constexpr std::uint32_t thread_set_access = 0x20 | 0x400;
inline constexpr std::uint32_t thread_resume_access = 2;
inline constexpr std::uint32_t thread_all_access = 0x1fffff;

struct ThreadIdentity {
    std::uint32_t id = 0;
    std::uint32_t error = 0;
};

struct ThreadPriority {
    std::int32_t value = 0x7fffffff;
    std::uint32_t error = 0;
};

struct ThreadResume {
    std::uint32_t previous = 0xffffffffu;
    std::uint32_t error = 0;
};

struct ThreadCreation {
    std::uintptr_t handle = 0;
    std::uint32_t id = 0;
    std::uint32_t error = 0;
};

class NativeThreads {
  public:
    static constexpr std::size_t stack_limit = 8 * 1024 * 1024;

    explicit NativeThreads(KernelObjects &objects)
        : objects_(objects), owner_native_id_(host::current_thread_id()) {}

    ~NativeThreads();
    ThreadCreation create(Runtime &runtime, pe::ModuleGraph &graph, ThreadEntry entry, void *argument,
                          std::size_t stack_bytes, bool suspended = false);
    std::uint32_t exit_code(std::uintptr_t handle, void *output) const;
    std::uint32_t thread_id(std::uintptr_t handle) const;
    ThreadIdentity identify(std::uintptr_t handle) const;
    ObjectResult open(std::uint32_t id, std::uint32_t access, bool inherited);
    void owner_returned(std::uint32_t result);
    ThreadResume resume(std::uintptr_t handle);
    std::uint32_t set_priority(std::uintptr_t handle, std::int32_t priority);
    ThreadPriority priority(std::uintptr_t handle) const;
    std::uint32_t set_priority_boost(std::uintptr_t handle, bool disabled) const;
    std::uint32_t group_affinity(std::uintptr_t handle, const ThreadAffinity &mapping,
                                 GroupAffinity &output) const;
    std::uint32_t set_group_affinity(std::uintptr_t handle, const ThreadAffinity &mapping,
                                     const GroupAffinity &value, GroupAffinity &previous);
    void shutdown() noexcept;

  private:
    struct Job;
    void reap_finished();
    std::uint32_t with_live_thread(std::uintptr_t handle, std::uint32_t required_access,
                                   const std::function<std::uint32_t(std::uint32_t)> &operation) const;
    KernelObjects &objects_;
    const std::thread::id owner_ = std::this_thread::get_id();
    const std::uint32_t owner_native_id_;
    std::mutex mutex_;
    bool closing_ = false;
    std::shared_ptr<WaitableObject> owner_completion_;
    std::optional<std::uint32_t> owner_result_;
    std::vector<std::shared_ptr<Job>> jobs_;
};

void register_thread_services(ImportRegistry &registry);
} // namespace ambrosia::services
