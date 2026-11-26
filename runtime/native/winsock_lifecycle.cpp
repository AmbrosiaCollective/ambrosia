#include "winsock_lifecycle.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace ambrosia::services {
std::int32_t WinsockLifecycle::startup(std::uint16_t requested, void *output) {
    if (!output) {
        return 10014; // WSAEFAULT: no initialization reference is acquired.
    }
    const auto major = requested & 255;
    const auto minor = requested >> 8;
    if (major < 2) {
        return 10092; // WSAVERNOTSUPPORTED: no legacy Winsock 1.x provider.
    }
    std::lock_guard lock(mutex_);
    if (references_ >= 4096) {
        return 10091; // WSASYSNOTREADY at this runtime's resource boundary.
    }
    WinsockData data;
    data.version = major == 2 ? static_cast<std::uint16_t>(2 | (std::min(minor, 2) << 8)) : 0x0202;
    constexpr char description[] = "Ambrosia offline Winsock compatibility";
    constexpr char status[] = "Offline; host socket transport disabled";
    std::memcpy(data.description.data(), description, sizeof(description));
    std::memcpy(data.status.data(), status, sizeof(status));
    std::memcpy(output, &data, sizeof(data));
    ++references_;
    return 0;
}

bool WinsockLifecycle::cleanup() {
    std::lock_guard lock(mutex_);
    if (!references_) {
        return false;
    }
    --references_;
    return true;
}

unsigned WinsockLifecycle::references() const {
    std::lock_guard lock(mutex_);
    return references_;
}

namespace {
struct RestoreNativeError {
    int saved = errno;

    ~RestoreNativeError() {
        errno = saved;
    }
};

WinsockLifecycle &winsock() {
    auto *runtime = current_runtime();
    if (!runtime) {
        throw std::runtime_error("Winsock initialization requires a scoped runtime");
    }
    return runtime->winsock();
}

std::int32_t AMBROSIA_GUEST_ABI startup(std::uint16_t version, void *output) noexcept {
    RestoreNativeError restore;
    try {
        return winsock().startup(version, output);
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

std::int32_t AMBROSIA_GUEST_ABI cleanup() noexcept {
    RestoreNativeError restore;
    try {
        if (winsock().cleanup()) {
            return 0;
        }
        set_last_error(10093); // WSANOTINITIALISED.
        return -1;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

std::int32_t AMBROSIA_GUEST_ABI last_error() noexcept {
    return std::bit_cast<std::int32_t>(get_last_error());
}

void AMBROSIA_GUEST_ABI store_error(std::int32_t error) noexcept {
    set_last_error(std::bit_cast<std::uint32_t>(error));
}
} // namespace

void register_winsock_lifecycle(ImportRegistry &registry) {
    constexpr auto module = "ws2_32.dll";
    registry.add(module, "WSAStartup", reinterpret_cast<std::uintptr_t>(&startup));
    registry.add(module, "WSACleanup", reinterpret_cast<std::uintptr_t>(&cleanup));
    registry.add(module, "WSAGetLastError", reinterpret_cast<std::uintptr_t>(&last_error));
    registry.add(module, "WSASetLastError", reinterpret_cast<std::uintptr_t>(&store_error));
    registry.add_ordinal(module, 115, "WSAStartup");
    registry.add_ordinal(module, 116, "WSACleanup");
    registry.add_ordinal(module, 111, "WSAGetLastError");
    registry.add_ordinal(module, 112, "WSASetLastError");
}
} // namespace ambrosia::services
