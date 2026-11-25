#include "math_services.h"
#include "service_runtime.h"
#include <cerrno>
#include <cmath>

namespace ambrosia::services {
namespace {
float AMBROSIA_GUEST_ABI tangent(float value) noexcept {
    const auto saved_error = errno;
    errno = 0;
    const auto result = std::tan(value);
    const auto operation_error = errno;
    // The default CRT math-error policy treats non-finite tangent inputs as
    // domain errors. Host libm need not set errno for a quiet NaN itself.
    if (!std::isfinite(value) || operation_error == EDOM) {
        crt_error() = 33;
    } else if (operation_error == ERANGE) {
        crt_error() = 34;
    }
    errno = saved_error;
    return result;
}
} // namespace

void register_math_services(ImportRegistry &registry) {
    for (const auto *module : {"ucrtbase.dll", "msvcrt.dll", "api-ms-win-crt-math-l1-1-0.dll"}) {
        registry.add(module, "tanf", reinterpret_cast<std::uintptr_t>(&tangent));
    }
}
} // namespace ambrosia::services
