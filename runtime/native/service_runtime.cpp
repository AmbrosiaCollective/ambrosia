#include "service_runtime.h"
#include "clock_services.h"
#include "critical_services.h"
#include "fls_services.h"
#include "graphics_services.h"
#include "guest_thread.h"
#include "kernel_services.h"
#include "loader_services.h"
#include "locale_services.h"
#include "registry_services.h"
#include "slist_services.h"
#include "xmem_services.h"
#include <array>
#include <stdexcept>

namespace ambrosia::services {
namespace {
struct NativeState {
    Runtime *runtime = nullptr;
    pe::ModuleGraph *modules = nullptr;
    std::uint32_t fallback_error = 0;
    std::int32_t fallback_crt_error = 0;
};

// Keep the native TLS resolver in a System V call on Linux. GCC can lose live
// guest arguments when it emits a dynamic TLS lookup directly inside ms_abi.
// Prevent inlining so this remains an ABI boundary in optimized/LTO builds.
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
NativeState &
native_state() noexcept {
    static thread_local NativeState state;
    return state;
}

void *AMBROSIA_GUEST_ABI virtual_alloc(void *address, std::size_t bytes, std::uint32_t flags,
                                       std::uint32_t protection) noexcept {
    auto *runtime = native_state().runtime;
    if (!runtime) {
        set_last_error(error_invalid_function);
        return nullptr;
    }
    const auto result = runtime->memory().allocate(address, bytes, flags, protection);
    if (result.error) {
        set_last_error(result.error);
    }
    return result.address;
}

std::int32_t AMBROSIA_GUEST_ABI virtual_free(void *address, std::size_t bytes, std::uint32_t flags) noexcept {
    auto *runtime = native_state().runtime;
    if (!runtime) {
        set_last_error(error_invalid_function);
        return 0;
    }
    const auto result = runtime->memory().free(address, bytes, flags);
    if (result.error) {
        set_last_error(result.error);
    }
    return result.success ? 1 : 0;
}

std::uint32_t AMBROSIA_GUEST_ABI last_error() noexcept {
    return get_last_error();
}

void AMBROSIA_GUEST_ABI store_error(std::uint32_t error) noexcept {
    set_last_error(error);
}
} // namespace

std::uint32_t get_last_error() noexcept {
    auto *thread = guest::ThreadContext::current();
    return thread ? thread->last_error() : native_state().fallback_error;
}

void set_last_error(std::uint32_t error) noexcept {
    auto *thread = guest::ThreadContext::current();
    if (thread) {
        thread->last_error() = error;
    } else {
        native_state().fallback_error = error;
    }
}

std::int32_t &crt_error() noexcept {
    auto *thread = guest::ThreadContext::current();
    return thread ? thread->crt_error() : native_state().fallback_crt_error;
}

Runtime::Runtime(std::size_t memory_budget)
    : Runtime(MemoryLimits{memory_budget, memory_budget, memory_budget}) {}

Runtime::Runtime(MemoryLimits memory_limits)
    : memory_(memory_limits), pointers_(host::random_pointer_secret()), objects_(4096, handle_ids_),
      registry_keys_(handle_ids_) {
    const auto locale_error = locale_sections_.initialize(locale_lock_.data(), 0, 0);
    if (locale_error) {
        throw std::runtime_error("CRT locale lock initialization failed");
    }
    register_clock_services(registry_);
    register_critical_services(registry_);
    register_fls_services(registry_);
    register_crt_lifecycle(registry_);
    register_slist_services(registry_);
    register_loader_services(registry_);
    register_crt_streams(registry_);
    register_crt_heap(registry_);
    register_locale_services(registry_);
    register_pointer_services(registry_);
    register_crt_startup(registry_);
    register_crt_exit(registry_);
    register_kernel_services(registry_);
    register_registry_services(registry_);
    register_graphics_services(registry_);
    register_xmem_services(registry_);
    // Alias list adapted from the inherited xmem.cpp title_memory table.
    constexpr std::array memory_modules = {"kernel32.dll",
                                           "kernelbase.dll",
                                           "api-ms-win-core-memory-l1-1-0.dll",
                                           "api-ms-win-core-memory-l1-1-1.dll",
                                           "api-ms-win-core-memory-l1-1-2.dll",
                                           "api-ms-win-core-memory-l1-1-3.dll",
                                           "api-ms-win-core-memory-l1-1-4.dll",
                                           "api-ms-win-core-memory-l1-1-5.dll",
                                           "api-ms-win-core-memory-l1-1-6.dll"};
    for (const auto *module : memory_modules) {
        registry_.add(module, "VirtualAlloc", reinterpret_cast<std::uintptr_t>(&virtual_alloc));
        registry_.add(module, "VirtualFree", reinterpret_cast<std::uintptr_t>(&virtual_free));
    }
    for (const auto *module :
         {"kernel32.dll", "kernelbase.dll", "api-ms-win-core-errorhandling-l1-1-0.dll"}) {
        registry_.add(module, "GetLastError", reinterpret_cast<std::uintptr_t>(&last_error));
        registry_.add(module, "SetLastError", reinterpret_cast<std::uintptr_t>(&store_error));
    }
    native_modules_ = std::make_unique<NativeModules>(registry_);
}

void Runtime::lock_locales() {
    (void)locale_sections_.enter(locale_lock_.data());
}

void Runtime::unlock_locales() {
    locale_sections_.leave(locale_lock_.data());
}

std::uintptr_t Runtime::resolve(const pe::Import &symbol) const {
    const auto target = registry_.resolve(symbol);
    return target ? native_modules_->entrypoint(target) : 0;
}

Runtime *current_runtime() noexcept {
    return native_state().runtime;
}

pe::ModuleGraph *current_modules() noexcept {
    return native_state().modules;
}

Scope::Scope(Runtime &runtime, pe::ModuleGraph *modules)
    : saved_(native_state().runtime), saved_modules_(native_state().modules) {
    native_state().runtime = &runtime;
    native_state().modules = modules;
}

Scope::~Scope() {
    native_state().runtime = saved_;
    native_state().modules = saved_modules_;
}
} // namespace ambrosia::services
