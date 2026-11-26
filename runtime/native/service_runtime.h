#pragma once
#include "app_lifecycle.h"
#include "critical_sections.h"
#include "crt_exit.h"
#include "crt_heap.h"
#include "crt_lifecycle.h"
#include "crt_locale.h"
#include "crt_locales.h"
#include "crt_startup.h"
#include "crt_streams.h"
#include "exception_filter.h"
#include "fls_slots.h"
#include "game_input.h"
#include "gdk_bootstrap.h"
#include "gdk_networking.h"
#include "gdk_packages.h"
#include "gdk_threading.h"
#include "gdk_users.h"
#include "graphics_memory_policy.h"
#include "import_registry.h"
#include "invalid_parameters.h"
#include "kernel_objects.h"
#include "library_references.h"
#include "memory_services.h"
#include "native_modules.h"
#include "native_threads.h"
#include "new_allocations.h"
#include "pointer_codec.h"
#include "processor_information.h"
#include "readonly_files.h"
#include "registry_keys.h"
#include "slim_locks.h"
#include "storage_factory.h"
#include "task_queues.h"
#include "winsock_lifecycle.h"
#include <functional>
#include <mutex>
#include <optional>

namespace ambrosia::services {
std::uint32_t get_last_error() noexcept;
void set_last_error(std::uint32_t error) noexcept;
std::int32_t &crt_error() noexcept;

class Runtime {
  public:
    explicit Runtime(std::size_t memory_budget = 64 * 1024 * 1024);
    explicit Runtime(MemoryLimits memory_limits, std::size_t heap_budget = 64 * 1024 * 1024);
    Runtime(MemoryLimits memory_limits, const host::ProcessorTopology &topology);
    ~Runtime();
    void shutdown_workers();
    // Capture on an initialized guest thread. Invoke only on this runtime's
    // joined native workers; each invocation owns fresh guest TLS/notifications.
    std::function<void()> capture_callback(std::function<void()> callback);
    std::uintptr_t resolve(const pe::Import &symbol) const;
    const ProcessorInformation &processor_information();

    bool has_service_module(std::string name) const {
        return native_modules_->has_module(std::move(name));
    }

    NativeModules &native_modules() const {
        return *native_modules_;
    }

    KernelObjects &objects() {
        return objects_;
    }

    LibraryReferences &library_references() {
        return library_references_;
    }

    GdkPackages &gdk_packages() {
        return gdk_packages_;
    }

    ReadonlyFiles &files() {
        return files_;
    }

    RegistryKeys &registry_keys() {
        return registry_keys_;
    }

    GraphicsMemoryPolicy &graphics_memory() {
        return graphics_memory_;
    }

    unsigned notify_app_lifecycle(AppNotification kind, bool value);

    NativeThreads &threads() {
        return threads_;
    }

    WinsockLifecycle &winsock() {
        return winsock_;
    }

    AppLifecycle &app_lifecycle() {
        return app_lifecycle_;
    }

    TaskQueues &task_queues() {
        return task_queues_;
    }

    StorageFactory &storage_factory() {
        return storage_factory_;
    }

    GameInput &game_input() {
        return game_input_;
    }

    GdkUsers &gdk_users() {
        return gdk_users_;
    }

    GdkNetworking &gdk_networking() {
        return gdk_networking_;
    }

    GdkThreading &gdk_threading() {
        return gdk_threading_;
    }

    GdkBootstrap &gdk_bootstrap() {
        return gdk_bootstrap_;
    }

    ExceptionFilter &exception_filter() {
        return exception_filter_;
    }

    MemoryServices &memory() {
        return memory_;
    }

    CriticalSections &critical_sections() {
        return critical_sections_;
    }

    SlimLocks &slim_locks() {
        return slim_locks_;
    }

    FlsSlots &fls() {
        return fls_;
    }

    OnExitTables &on_exit() {
        return on_exit_;
    }

    StandardStreams &streams() {
        return streams_;
    }

    CrtHeap &heap() {
        return heap_;
    }

    InvalidParameters &invalid_parameters() {
        return invalid_parameters_;
    }

    NewAllocations &new_allocations() {
        return new_allocations_;
    }

    void lock_locales();
    void unlock_locales();

    CrtLocale &locale() {
        return locales_.current();
    }

    std::int32_t configure_thread_locale(std::int32_t mode) {
        return locales_.configure(mode);
    }

    CrtStartup &startup() {
        return startup_;
    }

    ExitCallbacks &exit_callbacks() {
        return exit_callbacks_;
    }

    const PointerCodec &pointers() const {
        return pointers_;
    }

  private:
    MemoryServices memory_;
    CriticalSections critical_sections_;
    SlimLocks slim_locks_;
    FlsSlots fls_;
    OnExitTables on_exit_;
    StandardStreams streams_;
    CrtHeap heap_;
    NewAllocations new_allocations_;
    InvalidParameters invalid_parameters_;
    CriticalSections locale_sections_;
    alignas(8) std::array<std::byte, 40> locale_lock_{};
    PointerCodec pointers_;
    CrtLocales locales_;
    CrtStartup startup_;
    ExitCallbacks exit_callbacks_;
    ImportRegistry registry_;
    std::shared_ptr<HandleIds> handle_ids_ = std::make_shared<HandleIds>();
    KernelObjects objects_;
    LibraryReferences library_references_;
    RegistryKeys registry_keys_;
    ReadonlyFiles files_;
    GraphicsMemoryPolicy graphics_memory_;
    ExceptionFilter exception_filter_;
    GdkBootstrap gdk_bootstrap_;
    GdkThreading gdk_threading_;
    GdkNetworking gdk_networking_;
    GdkUsers gdk_users_;
    GdkPackages gdk_packages_;
    GameInput game_input_;
    StorageFactory storage_factory_;
    TaskQueues task_queues_;
    AppLifecycle app_lifecycle_;
    WinsockLifecycle winsock_;
    std::mutex processor_mutex_;
    std::optional<ProcessorInformation> processor_information_;
    std::unique_ptr<NativeModules> native_modules_;
    NativeThreads threads_;
};

Runtime *current_runtime() noexcept;
pe::ModuleGraph *current_modules() noexcept;

// The host-side service context is scoped independently of guest GS/TEB state.
// It must enclose module initialization, entry and detach notifications.
class Scope {
  public:
    explicit Scope(Runtime &runtime, pe::ModuleGraph *modules = nullptr);
    ~Scope();
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;
    Scope(Scope &&) = delete;
    Scope &operator=(Scope &&) = delete;

  private:
    Runtime *saved_;
    pe::ModuleGraph *saved_modules_;
};
} // namespace ambrosia::services
