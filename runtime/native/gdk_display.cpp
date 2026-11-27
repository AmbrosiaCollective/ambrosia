#include "gdk_display.h"
#include "guest_thread.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include <array>
#include <cerrno>
#include <stdexcept>

namespace ambrosia::services {
namespace {
struct RestoreNativeError {
    int saved = errno;

    ~RestoreNativeError() {
        errno = saved;
    }
};

std::uint32_t AMBROSIA_GUEST_ABI try_hdr(void *self, std::uint32_t preference, void *) noexcept {
    RestoreNativeError restore;
    try {
        auto *runtime = current_runtime();
        const auto *thread = guest::ThreadContext::current();
        if (!runtime || !thread || thread->time_sensitive()) {
            throw std::runtime_error("display HDR negotiation requires an ordinary active guest thread");
        }
        const auto result = runtime->gdk_display().try_enable_hdr(self, preference);
        // Luminance output is specified only when Enabled. An SDR-only backend
        // leaves the optional buffer untouched and supplies no fictional nits.
        if (result == DisplayHdrResult::disabled) {
            diagnostics::write_service_trace("TRACE native SDR-only output policy leaves HDR disabled\n");
        }
        return static_cast<std::uint32_t>(result);
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

[[noreturn]] void AMBROSIA_GUEST_ABI unsupported_display_slot() noexcept {
    diagnostics::stop_trace("unsupported display interface slot 3", 78);
}

const std::array<Guid, 1> interfaces{display_interface_id};
const std::array<std::uintptr_t, 5> vtable{reinterpret_cast<std::uintptr_t>(&interface_query),
                                           reinterpret_cast<std::uintptr_t>(&interface_add_reference),
                                           reinterpret_cast<std::uintptr_t>(&interface_release),
                                           reinterpret_cast<std::uintptr_t>(&unsupported_display_slot),
                                           reinterpret_cast<std::uintptr_t>(&try_hdr)};
} // namespace

GdkDisplay::GdkDisplay() : object_(vtable.data(), display_class_id, interfaces) {}

void GdkDisplay::select_sdr_output() {
    bool expected = false;
    if (!sdr_selected_.compare_exchange_strong(expected, true)) {
        throw std::runtime_error("native display output policy is already selected");
    }
}

DisplayHdrResult GdkDisplay::try_enable_hdr(const void *self, std::uint32_t preference) const {
    if (self != &object_) {
        throw std::runtime_error("display interface belongs to another runtime");
    }
    if (preference > 1 || !sdr_selected_.load()) {
        return DisplayHdrResult::unknown;
    }
    return DisplayHdrResult::disabled;
}

std::int32_t GdkDisplay::query(const Guid &interface_id, void *output) noexcept {
    return interface_query(&object_, &interface_id, output);
}
} // namespace ambrosia::services
