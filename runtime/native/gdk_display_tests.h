std::uint64_t AMBROSIA_GUEST_ABI display_policy_entry() {
    using namespace ambrosia::services;
    auto *runtime = current_runtime();
    if (!runtime || runtime->gdk_bootstrap().initialize(1, 0)) {
        return 99;
    }
    using Query = std::int32_t(AMBROSIA_GUEST_ABI *)(const Guid *, const Guid *, void **);
    using Hdr = std::uint32_t(AMBROSIA_GUEST_ABI *)(void *, std::uint32_t, void *);
    using Release = std::uint32_t(AMBROSIA_GUEST_ABI *)(void *);
    const auto query =
        reinterpret_cast<Query>(runtime->resolve(service_symbol("QueryApiImpl", "xgameruntime.dll")));
    void *object = nullptr;
    if (query(&display_class_id, &display_interface_id, &object) || !object) {
        return 98;
    }
    const auto table = *static_cast<const std::uintptr_t **>(object);
    const auto result = reinterpret_cast<Hdr>(table[4])(object, 0, nullptr);
    (void)reinterpret_cast<Release>(table[2])(object);
    runtime->gdk_bootstrap().uninitialize();
    return result;
}

void display_checks() {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    Runtime independent;
    Scope scope(runtime);
    guest::ThreadContext context;
    auto activation = context.activate();
    check(runtime.gdk_bootstrap().initialize(1, 0) == 0,
          "display queries use real offline bootstrap initialization");
    using Query = std::int32_t(AMBROSIA_GUEST_ABI *)(const Guid *, const Guid *, void **);
    using ObjectQuery = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, const Guid *, void **);
    using Release = std::uint32_t(AMBROSIA_GUEST_ABI *)(void *);
    using Hdr = std::uint32_t(AMBROSIA_GUEST_ABI *)(void *, std::uint32_t, void *);
    const auto query =
        reinterpret_cast<Query>(runtime.resolve(service_symbol("QueryApiImpl", "xgameruntime.dll")));
    void *object = nullptr;
    check(query(&display_class_id, &display_interface_id, &object) == 0 && object,
          "the measured display class/interface resolves to an owned counted native object");
    const auto table = *static_cast<const std::uintptr_t **>(object);
    const auto hdr = reinterpret_cast<Hdr>(table[4]);
    const auto object_query = reinterpret_cast<ObjectQuery>(table[0]);
    const auto release = reinterpret_cast<Release>(table[2]);
    void *identity = nullptr;
    check(object_query(object, &unknown_interface_id, &identity) == 0 && identity == object &&
              release(identity) == 2,
          "display IUnknown retains the same identity and real guest reference count");
    const Guid unknown{1, 2, 3, {4, 5, 6, 7, 8, 9, 10, 11}};
    identity = object;
    check(query(&display_class_id, &unknown, &identity) == no_interface && !identity &&
              runtime.gdk_display().query(display_interface_id, nullptr) == pointer_error,
          "unrelated display IIDs and missing query outputs fail without a placeholder object");
    std::array<std::byte, 14> info{};
    info.fill(std::byte{0xcc});
    const auto saved_errno = errno;
    errno = EDOM;
    crt_error() = 31;
    set_last_error(91);
    check(hdr(object, 0, info.data() + 1) == 0 && hdr(object, 1, nullptr) == 0 &&
              std::all_of(info.begin(), info.end(), [](auto value) { return value == std::byte{0xcc}; }),
          "an unconfigured output policy reports Unknown and never invents HDR luminance");
    runtime.gdk_display().select_sdr_output();
    check(hdr(object, 0, info.data() + 1) == 2 && hdr(object, 1, nullptr) == 2 &&
              hdr(object, 0, nullptr) == 2,
          "explicit SDR-only software output keeps HDR disabled for either valid preference");
    check(hdr(object, 2, info.data() + 1) == 0 && hdr(object, UINT32_MAX, nullptr) == 0 &&
              std::all_of(info.begin(), info.end(), [](auto value) { return value == std::byte{0xcc}; }),
          "invalid preferences report Unknown and disabled/error paths preserve optional output and guards");
    check(errno == EDOM && crt_error() == 31 && get_last_error() == 91 &&
              runtime_rejected([&] { runtime.gdk_display().select_sdr_output(); }),
          "HDR policy queries preserve native and guest errors and selection occurs once");
    {
        Scope other_scope(independent);
        void *other = nullptr;
        independent.gdk_display().query(display_interface_id, &other);
        check(hdr(other, 0, nullptr) == 0 &&
                  runtime_rejected([&] { (void)independent.gdk_display().try_enable_hdr(object, 0); }),
              "separate runtime policies remain independent and reject a foreign display object");
        check(reinterpret_cast<Release>((*static_cast<const std::uintptr_t **>(other))[2])(other) == 1,
              "independent display guest references return to their owning runtime pin");
    }
    check(release(object) == 1,
          "display guest release retains the runtime pin rather than destroying the backing object");
    auto bytes = fixture();
    put<std::uint32_t>(bytes, 0x98 + 120, 0);
    put<std::uint32_t>(bytes, 0x98 + 124, 0);
    jump_to_host(bytes, 0x200, reinterpret_cast<std::uintptr_t>(&display_policy_entry));
    const auto root = std::filesystem::path("/work/display-policy-command");
    std::filesystem::create_directories(root);
    const auto image = root / "original.exe";
    {
        std::ofstream file(image, std::ios::binary);
        file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    std::ostringstream output;
    std::ostringstream errors;
    check(run_native_command(std::vector<std::filesystem::path>{"--run-bootstrap", image, "--package", root},
                             output, errors) == 0 &&
              errors.str().empty() && output.str().find("\"bootstrap_result\":2") != std::string::npos,
          "the actual native command selects SDR policy before an original PE requests the display method");
    errno = saved_errno;
}

int display_rejection_probe(unsigned mode) {
    using namespace ambrosia;
    using namespace services;
    using Hdr = std::uint32_t(AMBROSIA_GUEST_ABI *)(void *, std::uint32_t, void *);
    Runtime runtime;
    Scope scope(runtime);
    runtime.gdk_display().select_sdr_output();
    void *object = nullptr;
    runtime.gdk_display().query(display_interface_id, &object);
    const auto table = *static_cast<const std::uintptr_t **>(object);
    const auto hdr = reinterpret_cast<Hdr>(table[4]);
    if (mode == 4) {
        (void)hdr(object, 0, nullptr);
        return 1;
    }
    guest::ThreadContext context;
    auto activation = context.activate();
    if (mode == 1) {
        context.set_time_sensitive(true);
        (void)hdr(object, 0, nullptr);
    } else if (mode == 2) {
        (void)hdr(nullptr, 0, nullptr);
    } else if (mode == 3) {
        using UnknownSlot = void(AMBROSIA_GUEST_ABI *)();
        reinterpret_cast<UnknownSlot>(table[3])();
    } else {
        return 2;
    }
    return 1;
}
