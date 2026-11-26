void neutral_region_checks() {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    using GeographicName = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, std::int32_t);
    const auto address = runtime.resolve(service_symbol("GetUserDefaultGeoName", "kernel32.dll"));
    const auto get = reinterpret_cast<GeographicName>(address);
    check(address && runtime.resolve(service_symbol("GetUserDefaultGeoName", "kernelbase.dll")) == address &&
              runtime.resolve(service_symbol("GetUserDefaultGeoName",
                                             "api-ms-win-core-localization-l1-2-3.dll")) == address,
          "user-geography aliases resolve the explicit neutral-world runtime policy");
    std::array<std::byte, 16> output;
    output.fill(std::byte{0xcc});
    const auto saved_error = errno;
    errno = EDOM;
    crt_error() = 61;
    set_last_error(91);
    const auto query = get(nullptr, 0);
    const auto nonnull_query = get(output.data() + 3, 0);
    check(query == 4 && nonnull_query == 4 && output[3] == std::byte{0xcc} && get_last_error() == 91 &&
              errno == EDOM && crt_error() == 61,
          "zero-capacity geography queries count UTF16 units including NUL without writing or disturbing "
          "error state");
    const auto copied = get(output.data() + 3, 4);
    constexpr char16_t expected[] = u"001";
    check(copied == 4 && !std::memcmp(output.data() + 3, expected, sizeof(expected)) &&
              output[2] == std::byte{0xcc} && output[11] == std::byte{0xcc} && get_last_error() == 91 &&
              errno == EDOM && crt_error() == 61,
          "exact-capacity unaligned geography output is the neutral world identifier and touches exactly "
          "four UTF16 units");
    output.fill(std::byte{0xcc});
    const auto insufficient = get(output.data() + 3, 3);
    check(!insufficient && get_last_error() == 122 &&
              std::all_of(output.begin(), output.end(), [](auto byte) { return byte == std::byte{0xcc}; }),
          "short geography output reports required buffer failure without a partial or unterminated write");
    const auto negative = get(output.data(), -1);
    const auto negative_error = get_last_error();
    const auto missing = get(nullptr, 4);
    check(!negative && negative_error == 87 && !missing && get_last_error() == 87 && errno == EDOM &&
              crt_error() == 61,
          "negative capacities and missing nonempty output report invalid parameters without leaking native "
          "error state");
    auto bytes = fixture();
    text(bytes, 0x440, "kernel32.dll");
    text(bytes, 0x4a2, "GetUserDefaultGeoName");
    const unsigned char code[] = {0x48, 0x83, 0xec, 0x28, 0x48, 0x8d, 0x0d, 0, 0,    0,    0,    0xba, 4,   0,
                                  0,    0,    0xff, 0x15, 0,    0,    0,    0, 0x48, 0x83, 0xc4, 0x28, 0xc3};
    std::memcpy(bytes.data() + 0x200, code, sizeof(code));
    put<std::uint32_t>(bytes, 0x207, 0x21f0 - 0x100b);
    put<std::uint32_t>(bytes, 0x212, 0x2080 - 0x1016);
    put<std::uint64_t>(bytes, 0x5f0, 0xccccccccccccccccull);
    auto image = pe::Image::load(bytes, [&](const auto &symbol) { return runtime.resolve(symbol); });
    const auto result = image.run_bootstrap();
    const auto *guest_output = image.base() + 0x21f0;
    check(result == 4 && !std::memcmp(guest_output, expected, sizeof(expected)),
          "original PE entry receives the neutral geographic identifier through its real Microsoft ABI "
          "import and output buffer");
    errno = saved_error;
}
