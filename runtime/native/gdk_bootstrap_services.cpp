#include "gdk_bootstrap.h"
#include "guest_strings.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace ambrosia::services {
namespace {
struct RestoreNativeError {
    int saved = errno;

    ~RestoreNativeError() {
        errno = saved;
    }
};

GdkBootstrap &bootstrap() {
    auto *runtime = current_runtime();
    if (!runtime) {
        throw std::runtime_error("GDK bootstrap requires a scoped runtime");
    }
    return runtime->gdk_bootstrap();
}

std::int32_t AMBROSIA_GUEST_ABI initialize(std::uint32_t gdk, std::uint32_t services) noexcept {
    RestoreNativeError restore;
    try {
        return bootstrap().initialize(gdk, services);
    } catch (const std::bad_alloc &) {
        return gdk_out_of_memory;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

std::int32_t AMBROSIA_GUEST_ABI initialize_ex(std::uint32_t gdk, std::uint32_t services,
                                              std::uint8_t mode) noexcept {
    if (mode != 2) {
        diagnostics::stop_trace("unmodeled GDK initialization mode", 78);
    }
    return initialize(gdk, services);
}

std::int32_t AMBROSIA_GUEST_ABI initialize_ex2(std::uint32_t gdk, std::uint32_t services, std::uint8_t mode,
                                               const void *options) noexcept {
    if (options) {
        diagnostics::stop_trace("GDK inline/default configuration options are not modeled", 78);
    }
    return initialize_ex(gdk, services, mode);
}

std::int32_t AMBROSIA_GUEST_ABI uninitialize() noexcept {
    RestoreNativeError restore;
    try {
        bootstrap().uninitialize();
        return 0;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

std::int32_t AMBROSIA_GUEST_ABI error_report(std::int32_t code, const char *message) noexcept {
    RestoreNativeError restore;
    try {
        return bootstrap().report_error(code, message ? guest::read_string(message) : std::string{});
    } catch (const std::bad_alloc &) {
        return gdk_out_of_memory;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

std::string guid_bytes(const void *source) {
    std::array<unsigned char, 16> bytes;
    std::memcpy(bytes.data(), source, bytes.size());
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(32);
    for (const auto value : bytes) {
        result.push_back(digits[value >> 4]);
        result.push_back(digits[value & 15]);
    }
    return result;
}

std::int32_t AMBROSIA_GUEST_ABI query_api(const void *class_id, const void *interface_id,
                                          void **output) noexcept {
    try {
        if (!class_id || !interface_id || !output || !bootstrap().snapshot().references) {
            diagnostics::stop_trace("GDK class query requires initialized bootstrap and valid arguments", 78);
        }
        void *empty = nullptr;
        std::memcpy(output, &empty, sizeof(empty));
        // No placeholder vtable or successful class result. Numeric request
        // metadata belongs in private startup evidence, never public game logs.
        diagnostics::write_service_trace("TRACE unsupported GDK class=" + guid_bytes(class_id) +
                                         " interface=" + guid_bytes(interface_id) + "\n");
        diagnostics::stop_trace("native GDK class service is not implemented", 78);
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}
} // namespace

void register_gdk_bootstrap(ImportRegistry &registry) {
    registry.add("xgameruntime.dll", "InitializeApiImpl", reinterpret_cast<std::uintptr_t>(&initialize));
    registry.add("xgameruntime.dll", "InitializeApiImplEx", reinterpret_cast<std::uintptr_t>(&initialize_ex));
    registry.add("xgameruntime.dll", "InitializeApiImplEx2",
                 reinterpret_cast<std::uintptr_t>(&initialize_ex2));
    registry.add("xgameruntime.dll", "UninitializeApiImpl", reinterpret_cast<std::uintptr_t>(&uninitialize));
    registry.add("xgameruntime.dll", "XErrorReport", reinterpret_cast<std::uintptr_t>(&error_report));
    registry.add("xgameruntime.dll", "QueryApiImpl", reinterpret_cast<std::uintptr_t>(&query_api));
}
} // namespace ambrosia::services
