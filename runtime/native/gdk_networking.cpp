#include "gdk_networking.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>

namespace ambrosia::services {
namespace {
template <unsigned Slot> [[noreturn]] void AMBROSIA_GUEST_ABI unsupported() noexcept {
    diagnostics::stop_trace("unsupported XNetworkingImpl slot " + std::to_string(Slot), 78);
}

GdkNetworking &networking() {
    auto *runtime = current_runtime();
    if (!runtime || !runtime->gdk_bootstrap().snapshot().references) {
        diagnostics::stop_trace("offline networking requires an initialized scoped GDK runtime", 78);
    }
    return runtime->gdk_networking();
}

std::int32_t AMBROSIA_GUEST_ABI query_hint(void *output) noexcept {
    try {
        return networking().connectivity_hint(output);
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

std::int32_t AMBROSIA_GUEST_ABI query_port(void *output) noexcept {
    try {
        return networking().preferred_udp_port(output);
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

std::int32_t AMBROSIA_GUEST_ABI interface_hint(void *, void *output) noexcept {
    return query_hint(output);
}

std::int32_t AMBROSIA_GUEST_ABI interface_port(void *, void *output) noexcept {
    return query_port(output);
}

const std::array interfaces{networking_v2_id};
const std::array<std::uintptr_t, 21> networking_vtable{
    reinterpret_cast<std::uintptr_t>(&interface_query),
    reinterpret_cast<std::uintptr_t>(&interface_add_reference),
    reinterpret_cast<std::uintptr_t>(&interface_release),
    reinterpret_cast<std::uintptr_t>(&interface_port),
    reinterpret_cast<std::uintptr_t>(&unsupported<4>),
    reinterpret_cast<std::uintptr_t>(&unsupported<5>),
    reinterpret_cast<std::uintptr_t>(&unsupported<6>),
    reinterpret_cast<std::uintptr_t>(&unsupported<7>),
    reinterpret_cast<std::uintptr_t>(&unsupported<8>),
    reinterpret_cast<std::uintptr_t>(&unsupported<9>),
    reinterpret_cast<std::uintptr_t>(&unsupported<10>),
    reinterpret_cast<std::uintptr_t>(&unsupported<11>),
    reinterpret_cast<std::uintptr_t>(&unsupported<12>),
    reinterpret_cast<std::uintptr_t>(&unsupported<13>),
    reinterpret_cast<std::uintptr_t>(&unsupported<14>),
    reinterpret_cast<std::uintptr_t>(&interface_hint),
    reinterpret_cast<std::uintptr_t>(&unsupported<16>),
    reinterpret_cast<std::uintptr_t>(&unsupported<17>),
    reinterpret_cast<std::uintptr_t>(&unsupported<18>),
    reinterpret_cast<std::uintptr_t>(&unsupported<19>),
    reinterpret_cast<std::uintptr_t>(&unsupported<20>)};
} // namespace

GdkNetworking::GdkNetworking() : object_(networking_vtable.data(), networking_class_id, interfaces) {}

std::int32_t GdkNetworking::query(const Guid &interface_id, void *output) noexcept {
    return interface_query(&object_, &interface_id, output);
}

std::int32_t GdkNetworking::connectivity_hint(void *output) const noexcept {
    if (!output) {
        return pointer_error;
    }
    std::memcpy(output, &hint_, sizeof(hint_));
    return 0;
}

std::int32_t GdkNetworking::preferred_udp_port(void *output) const noexcept {
    if (!output) {
        return pointer_error;
    }
    const std::uint16_t unavailable = 0;
    std::memcpy(output, &unavailable, sizeof(unavailable));
    return network_unreachable;
}

void register_networking_services(ImportRegistry &registry) {
    registry.add("xgameruntime.dll", "XNetworkingGetConnectivityHint",
                 reinterpret_cast<std::uintptr_t>(&query_hint));
    registry.add("xgameruntime.dll", "XNetworkingQueryPreferredLocalUdpMultiplayerPort",
                 reinterpret_cast<std::uintptr_t>(&query_port));
}
} // namespace ambrosia::services
