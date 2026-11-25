#include "random_services.h"
#include "host.h"
#include "startup_trace.h"
#include <cerrno>
#include <cstring>

namespace ambrosia::services {
namespace {
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
    }
}
} // namespace ambrosia::services
