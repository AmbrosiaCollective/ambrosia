void graphics_memory_checks() {
    using namespace ambrosia::services;
    constexpr auto invalid_argument = static_cast<std::int32_t>(0x80070057u);
    constexpr auto invalid_call = static_cast<std::int32_t>(0x887a0001u);
    const auto description = [](std::uint32_t range_class) {
        std::array<std::byte, 32> bytes{};
        std::memcpy(bytes.data() + 4, &range_class, sizeof(range_class));
        return bytes;
    };
    GraphicsMemoryPolicy policy;
    check(!policy.snapshot() && policy.configure({}) == invalid_argument &&
              policy.configure(std::array<std::byte, 31>{}) == invalid_argument && !policy.snapshot(),
          "graphics configuration starts empty and rejects incorrect descriptor lengths without mutation");
    bool refused_classes = true;
    for (const auto range_class : {0u, 1u, 2u, 5u, 0x80000003u, 0xffffffffu}) {
        refused_classes = refused_classes && policy.configure(description(range_class)) == invalid_argument;
    }
    check(refused_classes && !policy.snapshot(),
          "graphics configuration refuses unobserved range classes without inventing SDK semantics");
    bool refused_fields = true;
    for (std::size_t index = 0; index < 32; ++index) {
        if (index >= 4 && index < 8) {
            continue;
        }
        auto bytes = description(3);
        bytes[index] = std::byte{1};
        refused_fields = refused_fields && policy.configure(bytes) == invalid_argument;
    }
    check(refused_fields && !policy.snapshot(),
          "unobserved descriptor fields must remain zero and rejected configurations retain no plan");
    const auto bytes = description(3);
    const auto original = bytes;
    check(policy.configure(bytes) == 0 && bytes == original && policy.snapshot()->range_class == 3 &&
              policy.snapshot()->covered_bytes == (16ull << 30),
          "known graphics class is retained with derived coverage without writing caller memory");
    check(policy.configure(description(4)) == invalid_call && policy.snapshot()->range_class == 3,
          "duplicate configuration fails without replacing the accepted policy");
    GraphicsMemoryPolicy larger;
    check(larger.configure(description(4)) == 0 && larger.snapshot()->covered_bytes == (32ull << 30),
          "independent graphics policies retain separate observed coverage classes");
    {
        auto creation = policy.begin_device_creation();
        check(creation.plan() && creation.plan()->range_class == 3,
              "serialized backend creation receives a value snapshot of the accepted policy");
        creation.succeeded();
    }
    check(policy.configure(description(3)) == invalid_call && policy.snapshot()->range_class == 3,
          "completed backend creation cannot reopen graphics configuration");
    GraphicsMemoryPolicy late;
    {
        auto creation = late.begin_device_creation();
        check(!creation.plan(), "backend creation can observe an absent optional graphics policy");
        creation.succeeded();
    }
    check(late.configure(description(4)) == invalid_call && !late.snapshot(),
          "configuration after a completed unconfigured device is rejected without recording a plan");
    GraphicsMemoryPolicy failed_creation;
    { auto creation = failed_creation.begin_device_creation(); }
    check(failed_creation.configure(description(4)) == 0,
          "failed backend creation releases its gate without claiming a successful device");

    Runtime runtime;
    Scope scope(runtime);
    using Configure = std::int32_t(AMBROSIA_GUEST_ABI *)(const void *);
    const auto configure = reinterpret_cast<Configure>(
        runtime.resolve(service_symbol("D3D12ConfigureVirtualMemoryX", "d3d12_xs.dll")));
    std::array<std::byte, 64> guarded;
    guarded.fill(std::byte{0xa5});
    const auto valid = description(4);
    std::copy(valid.begin(), valid.end(), guarded.begin() + 16);
    const auto before = guarded;
    set_last_error(67);
    crt_error() = 29;
    check(configure(nullptr) == static_cast<std::int32_t>(0x80004003u) &&
              !runtime.graphics_memory().snapshot() && get_last_error() == 67 && crt_error() == 29,
          "graphics ABI rejects a null descriptor with E_POINTER and preserves independent thread errors");
    check(
        configure(guarded.data() + 16) == 0 && guarded == before &&
            runtime.graphics_memory().snapshot()->range_class == 4 && get_last_error() == 67 &&
            crt_error() == 29,
        "graphics Microsoft ABI records configuration without modifying descriptor or surrounding canaries");
    guarded[16] = std::byte{1};
    check(configure(guarded.data() + 16) == invalid_argument &&
              runtime.graphics_memory().snapshot()->range_class == 4,
          "graphics ABI validates malformed fields without overwriting its accepted plan");
    check(configure(valid.data()) == invalid_call && get_last_error() == 67 && crt_error() == 29,
          "graphics ABI rejects repeat calls with HRESULT rather than changing last-error or errno");
}
