void gdk_networking_checks() {
    using namespace ambrosia::services;
    Runtime runtime;
    Runtime independent;
    Scope scope(runtime);
    (void)runtime.gdk_bootstrap().initialize(1, 2);
    using Query = std::int32_t(AMBROSIA_GUEST_ABI *)(const void *, const void *, void *);
    const auto query =
        reinterpret_cast<Query>(runtime.resolve(service_symbol("QueryApiImpl", "xgameruntime.dll")));
    void *object = nullptr;
    const auto requested = query(&networking_class_id, &networking_v2_id, &object);
    check(requested == 0 && object && runtime.gdk_networking().references() == 2 &&
              independent.gdk_networking().references() == 1,
          "the GDK networking v2 class owns its real queried interface independently per runtime");
    const auto *table = *static_cast<const std::uintptr_t *const *>(object);
    using InterfaceQuery = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, const void *, void *);
    using Release = std::uint32_t(AMBROSIA_GUEST_ABI *)(void *);
    using Method = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, void *);
    using Flat = std::int32_t(AMBROSIA_GUEST_ABI *)(void *);
    const auto qi = reinterpret_cast<InterfaceQuery>(table[0]);
    const auto release = reinterpret_cast<Release>(table[2]);
    const auto port = reinterpret_cast<Method>(table[3]);
    const auto hint = reinterpret_cast<Method>(table[15]);
    void *base = nullptr;
    const auto base_result = qi(object, &networking_class_id, &base);
    check(base_result == 0 && base == object && runtime.gdk_networking().references() == 3,
          "base and v2 networking IDs share one counted Windows interface identity");
    (void)release(base);
    const Guid absent{};
    base = reinterpret_cast<void *>(1);
    check(qi(object, &absent, &base) == no_interface && !base,
          "unknown networking revisions are rejected without inventing a vtable");
    const auto flat_hint = reinterpret_cast<Flat>(
        runtime.resolve(service_symbol("XNetworkingGetConnectivityHint", "xgameruntime.dll")));
    const auto flat_port = reinterpret_cast<Flat>(runtime.resolve(
        service_symbol("XNetworkingQueryPreferredLocalUdpMultiplayerPort", "xgameruntime.dll")));
    const auto saved_error = errno;
    errno = EDOM;
    crt_error() = 61;
    set_last_error(37);
    check(hint(object, nullptr) == pointer_error && port(object, nullptr) == pointer_error &&
              flat_hint(nullptr) == pointer_error && flat_port(nullptr) == pointer_error,
          "flat and interface network queries return E_POINTER for missing output buffers");
    std::array<std::byte, 24> guarded;
    guarded.fill(std::byte{0xcc});
    const auto status = hint(object, guarded.data() + 3);
    ConnectivityHint observed;
    std::memcpy(&observed, guarded.data() + 3, sizeof(observed));
    check(status == 0 && observed.level == 1 && observed.cost == 0 && observed.interface_type == 0 &&
              observed.initialized == 1 && observed.approaching_limit == 0 && observed.over_limit == 0 &&
              observed.roaming == 0 && guarded[2] == std::byte{0xcc} && guarded[19] == std::byte{0xcc},
          "the exact 16-byte Windows hint reports an initialized offline provider and preserves unaligned "
          "guards");
    ConnectivityHint flat;
    const auto flat_status = flat_hint(&flat);
    check(flat_status == 0 && std::memcmp(&flat, &observed, sizeof(flat)) == 0,
          "flat and interface queries return the same offline policy without probing host connectivity");
    guarded.fill(std::byte{0xcc});
    const auto unavailable = port(object, guarded.data() + 3);
    std::uint16_t observed_port = 99;
    std::memcpy(&observed_port, guarded.data() + 3, sizeof(observed_port));
    check(unavailable == network_unreachable && observed_port == 0 && guarded[2] == std::byte{0xcc} &&
              guarded[5] == std::byte{0xcc},
          "the offline UDP query clears exactly two bytes and reports network unreachable instead of a fake "
          "port");
    observed_port = 99;
    check(flat_port(&observed_port) == network_unreachable && observed_port == 0,
          "the flat UDP query follows the same offline failure policy");
    check(errno == EDOM && crt_error() == 61 && get_last_error() == 37,
          "network HRESULT queries preserve native errno and guest error channels");
    check(release(object) == 1 && runtime.gdk_networking().references() == 1,
          "networking releases retain the runtime owner pin without leaking queried references");
    errno = saved_error;
}
