#include "random_services.h"
#include "guest_thread.h"
#include "host.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace ambrosia::services {
namespace {
guest::ThreadContext &random_context() {
    auto *context = guest::ThreadContext::current();
    if (!current_runtime() || !context) {
        throw std::runtime_error("CRT PRNG requires an active guest thread and runtime");
    }
    return *context;
}

void AMBROSIA_GUEST_ABI seed_random(std::uint32_t seed) noexcept {
    const auto saved_error = errno;
    try {
        random_context().seed_crt_random(seed);
        diagnostics::write_service_trace("TRACE native CRT PRNG seeded\n");
        errno = saved_error;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

std::int32_t AMBROSIA_GUEST_ABI random_integer() noexcept {
    const auto saved_error = errno;
    try {
        const auto result = random_context().next_crt_random();
        diagnostics::write_service_trace("TRACE native CRT PRNG advanced\n");
        errno = saved_error;
        return result;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

std::int32_t AMBROSIA_GUEST_ABI secure_random(void *output) noexcept {
    if (!output) {
        diagnostics::stop_trace("rand_s invalid-parameter dispatch is unsupported", 78);
    }
    const auto saved_error = errno;
    try {
        const auto value = host::secure_random_u32();
        std::memcpy(output, &value, sizeof(value));
        errno = saved_error;
        return 0;
    } catch (...) {
        const std::uint32_t zero = 0;
        std::memcpy(output, &zero, sizeof(zero));
        errno = saved_error;
        // Full CRT entropy-failure/invalid-handler policy is not modeled. Fail
        // before returning success or exposing a partial/uninitialized value.
        diagnostics::stop_trace("rand_s OS entropy failure", 78);
    }
}
} // namespace

void register_random_services(ImportRegistry &registry) {
    for (const auto *module : {"ucrtbase.dll", "msvcrt.dll", "api-ms-win-crt-utility-l1-1-0.dll"}) {
        registry.add(module, "rand_s", reinterpret_cast<std::uintptr_t>(&secure_random));
        registry.add(module, "srand", reinterpret_cast<std::uintptr_t>(&seed_random));
        registry.add(module, "rand", reinterpret_cast<std::uintptr_t>(&random_integer));
    }
}
} // namespace ambrosia::services
