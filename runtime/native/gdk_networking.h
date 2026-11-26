#pragma once
#include "runtime_interface.h"

namespace ambrosia::services {
inline constexpr Guid networking_class_id{
    0x37e56907, 0x2f10, 0x41e8, {0xb7, 0x2f, 0x36, 0xed, 0xb1, 0x85, 0x33, 0x1a}};
inline constexpr Guid networking_v2_id{
    0xbf2346b2, 0x39af, 0x4658, {0xb5, 0xea, 0x44, 0x71, 0x3c, 0x7e, 0x83, 0xb3}};
inline constexpr std::int32_t network_unreachable = static_cast<std::int32_t>(0x800704cfu);

struct ConnectivityHint {
    std::uint32_t level = 1; // None, not unknown or internet access.
    std::uint32_t cost = 0;
    std::uint32_t interface_type = 0;
    std::uint8_t initialized = 1;
    std::uint8_t approaching_limit = 0;
    std::uint8_t over_limit = 0;
    std::uint8_t roaming = 0;
};

static_assert(sizeof(ConnectivityHint) == 16 && offsetof(ConnectivityHint, initialized) == 12);

// This provider is initialized in offline-only mode. It never examines host
// interfaces, acquires a socket, reserves a port or claims online connectivity.
class GdkNetworking {
  public:
    GdkNetworking();
    std::int32_t query(const Guid &interface_id, void *output) noexcept;
    std::int32_t connectivity_hint(void *output) const noexcept;
    std::int32_t preferred_udp_port(void *output) const noexcept;

    std::uint32_t references() const noexcept {
        return object_.references.load();
    }

  private:
    RuntimeInterface object_;
    const ConnectivityHint hint_;
};

void register_networking_services(ImportRegistry &registry);
} // namespace ambrosia::services
