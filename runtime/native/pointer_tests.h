void pointer_codec_checks() {
    using namespace ambrosia::services;
    PointerCodec first(0x123456789abcdef0ull);
    PointerCodec second(0xfedcba9876543210ull);
    bool refused_zero = false;
    try {
        PointerCodec invalid(0);
    } catch (const std::invalid_argument &) {
        refused_zero = true;
    }
    check(refused_zero, "pointer codec refuses a zero secret");
    const std::array<std::uintptr_t, 6> values{
        0, 1, 0x12345678, UINTPTR_MAX, UINTPTR_MAX / 2, std::uintptr_t{1} << 63};
    bool round_trip = true;
    bool distinct = true;
    for (const auto value : values) {
        const auto encoded = first.encode(value);
        round_trip = round_trip && first.decode(encoded) == value;
        distinct = distinct && encoded != second.encode(value);
    }
    check(round_trip, "pointer encoding round-trips null and all x64 edge bit patterns");
    check(distinct, "different pointer secrets produce different reversible encodings");
    check(first.encode(0) != 0 && first.decode(first.encode(0)) == 0,
          "encoded null is opaque while decoding restores null");
    // The host adapter refuses unavailable entropy instead of falling back to
    // a fixed constant or host address. Its actual secret is never reported.
    check(ambrosia::host::random_pointer_secret() != 0,
          "installed host entropy adapter supplies a nonzero secret under isolation");
    Runtime runtime;
    using Transform = void *(AMBROSIA_GUEST_ABI *)(void *);
    const auto encode = reinterpret_cast<Transform>(
        runtime.resolve(service_symbol("EncodePointer", "api-ms-win-core-util-l1-1-0.dll")));
    const auto decode =
        reinterpret_cast<Transform>(runtime.resolve(service_symbol("DecodePointer", "kernel32.dll")));
    Scope scope(runtime);
    std::uint64_t object = 0x1357;
    set_last_error(57);
    crt_error() = 21;
    auto *encoded = encode(&object);
    check(decode(encoded) == &object && object == 0x1357 && encode(&object) == encoded,
          "Microsoft ABI pointer transforms are stable, reversible and never dereference values");
    check(decode(encode(nullptr)) == nullptr && encode(nullptr),
          "pointer service round-trips an encoded null");
    check(get_last_error() == 57 && crt_error() == 21, "pointer transforms preserve Win32 and CRT errors");
    check(runtime.resolve(service_symbol("EncodePointer", "kernelbase.dll")) ==
              reinterpret_cast<std::uintptr_t>(encode),
          "Win32 and API-set pointer encoding aliases bind the same adapter");
}
