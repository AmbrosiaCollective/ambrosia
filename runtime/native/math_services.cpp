#include "math_services.h"
#include "service_runtime.h"
#include <cerrno>
#include <cmath>

namespace ambrosia::services {
namespace {
// libm uses native errno. Guest CRT errno belongs to the active runtime/thread;
// successful calculations must preserve both it and the caller's native errno.
class MathErrorScope {
  public:
    MathErrorScope() : saved_error_(errno) {
        errno = 0;
    }

    ~MathErrorScope() {
        if (errno == EDOM) {
            crt_error() = 33;
        } else if (errno == ERANGE) {
            crt_error() = 34;
        }
        errno = saved_error_;
    }

  private:
    int saved_error_;
};

template <class Number, Number (*Operation)(Number)> Number AMBROSIA_GUEST_ABI unary(Number value) noexcept {
    MathErrorScope errors;
    return Operation(value);
}

template <class Number, Number (*Operation)(Number, Number)>
Number AMBROSIA_GUEST_ABI binary(Number first, Number second) noexcept {
    MathErrorScope errors;
    return Operation(first, second);
}

template <class Number, Number (*Operation)(Number, Number *)>
Number AMBROSIA_GUEST_ABI split_fraction(Number value, Number *integral) noexcept {
    MathErrorScope errors;
    return Operation(value, integral);
}

template <class Number> Number AMBROSIA_GUEST_ABI tangent(Number value) noexcept {
    MathErrorScope errors;
    const auto result = std::tan(value);
    // The default CRT tangent policy also treats quiet NaNs as domain errors.
    // Keep libm's real floating-point flags rather than synthesizing a trap.
    if (!std::isfinite(value)) {
        errno = EDOM;
    }
    return result;
}
} // namespace

void register_math_services(ImportRegistry &registry) {
    for (const auto *module : {"ucrtbase.dll", "msvcrt.dll", "api-ms-win-crt-math-l1-1-0.dll"}) {
        registry.add(module, "acos", reinterpret_cast<std::uintptr_t>(&unary<double, std::acos>));
        registry.add(module, "acosf", reinterpret_cast<std::uintptr_t>(&unary<float, std::acos>));
        registry.add(module, "asin", reinterpret_cast<std::uintptr_t>(&unary<double, std::asin>));
        registry.add(module, "asinf", reinterpret_cast<std::uintptr_t>(&unary<float, std::asin>));
        registry.add(module, "atan", reinterpret_cast<std::uintptr_t>(&unary<double, std::atan>));
        registry.add(module, "atanf", reinterpret_cast<std::uintptr_t>(&unary<float, std::atan>));
        registry.add(module, "atan2", reinterpret_cast<std::uintptr_t>(&binary<double, std::atan2>));
        registry.add(module, "atan2f", reinterpret_cast<std::uintptr_t>(&binary<float, std::atan2>));
        registry.add(module, "cbrtf", reinterpret_cast<std::uintptr_t>(&unary<float, std::cbrt>));
        registry.add(module, "ceilf", reinterpret_cast<std::uintptr_t>(&unary<float, std::ceil>));
        registry.add(module, "cos", reinterpret_cast<std::uintptr_t>(&unary<double, std::cos>));
        registry.add(module, "cosf", reinterpret_cast<std::uintptr_t>(&unary<float, std::cos>));
        registry.add(module, "erff", reinterpret_cast<std::uintptr_t>(&unary<float, std::erf>));
        registry.add(module, "exp", reinterpret_cast<std::uintptr_t>(&unary<double, std::exp>));
        registry.add(module, "expf", reinterpret_cast<std::uintptr_t>(&unary<float, std::exp>));
        registry.add(module, "exp2", reinterpret_cast<std::uintptr_t>(&unary<double, std::exp2>));
        registry.add(module, "exp2f", reinterpret_cast<std::uintptr_t>(&unary<float, std::exp2>));
        registry.add(module, "fmod", reinterpret_cast<std::uintptr_t>(&binary<double, std::fmod>));
        registry.add(module, "fmodf", reinterpret_cast<std::uintptr_t>(&binary<float, std::fmod>));
        registry.add(module, "log", reinterpret_cast<std::uintptr_t>(&unary<double, std::log>));
        registry.add(module, "logf", reinterpret_cast<std::uintptr_t>(&unary<float, std::log>));
        registry.add(module, "log10f", reinterpret_cast<std::uintptr_t>(&unary<float, std::log10>));
        registry.add(module, "log2", reinterpret_cast<std::uintptr_t>(&unary<double, std::log2>));
        registry.add(module, "log2f", reinterpret_cast<std::uintptr_t>(&unary<float, std::log2>));
        registry.add(module, "modf", reinterpret_cast<std::uintptr_t>(&split_fraction<double, std::modf>));
        registry.add(module, "modff", reinterpret_cast<std::uintptr_t>(&split_fraction<float, std::modf>));
        registry.add(module, "nextafterf", reinterpret_cast<std::uintptr_t>(&binary<float, std::nextafter>));
        registry.add(module, "pow", reinterpret_cast<std::uintptr_t>(&binary<double, std::pow>));
        registry.add(module, "powf", reinterpret_cast<std::uintptr_t>(&binary<float, std::pow>));
        registry.add(module, "remainderf", reinterpret_cast<std::uintptr_t>(&binary<float, std::remainder>));
        registry.add(module, "sin", reinterpret_cast<std::uintptr_t>(&unary<double, std::sin>));
        registry.add(module, "sinf", reinterpret_cast<std::uintptr_t>(&unary<float, std::sin>));
        registry.add(module, "sinhf", reinterpret_cast<std::uintptr_t>(&unary<float, std::sinh>));
        registry.add(module, "tan", reinterpret_cast<std::uintptr_t>(&tangent<double>));
        registry.add(module, "tanf", reinterpret_cast<std::uintptr_t>(&tangent<float>));
    }
}
} // namespace ambrosia::services
