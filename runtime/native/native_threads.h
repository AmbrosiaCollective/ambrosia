#pragma once
#include "kernel_objects.h"
#include "module_graph.h"
#include <thread>
#include <vector>

namespace ambrosia::services {
class Runtime;
class ImportRegistry;
using ThreadEntry = std::uint32_t(AMBROSIA_GUEST_ABI *)(void *);

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

    explicit NativeThreads(KernelObjects &objects) : objects_(objects) {}

    ~NativeThreads();
    ThreadCreation create(Runtime &runtime, pe::ModuleGraph &graph, ThreadEntry entry, void *argument,
                          std::size_t stack_bytes, bool suspended = false);
    std::uint32_t exit_code(std::uintptr_t handle, void *output) const;
    std::uint32_t thread_id(std::uintptr_t handle) const;
    ThreadResume resume(std::uintptr_t handle);
    void shutdown() noexcept;

  private:
    struct Job;
    void reap_finished();
    KernelObjects &objects_;
    const std::thread::id owner_ = std::this_thread::get_id();
    std::mutex mutex_;
    bool closing_ = false;
    std::vector<std::shared_ptr<Job>> jobs_;
};

void register_thread_services(ImportRegistry &registry);
} // namespace ambrosia::services
