#include "service_runtime.h"
#include "clock_services.h"
#include "critical_services.h"
#include "fls_services.h"
#include "guest_thread.h"
#include <array>

namespace ambrosia::services {
namespace {
struct NativeState {
    Runtime *runtime = nullptr;
    std::uint32_t fallback_error = 0;
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

Runtime::Runtime(std::size_t memory_budget) : memory_(memory_budget) {
    register_clock_services(registry_);
    register_critical_services(registry_);
    register_fls_services(registry_);
    register_crt_lifecycle(registry_);
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
}

std::uintptr_t Runtime::resolve(const pe::Import &symbol) const {
    return registry_.resolve(symbol);
}

Runtime *current_runtime() noexcept {
    return native_state().runtime;
}

Scope::Scope(Runtime &runtime) : saved_(native_state().runtime) {
    native_state().runtime = &runtime;
}

Scope::~Scope() {
    native_state().runtime = saved_;
}
} // namespace ambrosia::services
