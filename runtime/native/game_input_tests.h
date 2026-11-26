unsigned input_callback_calls = 0;

void AMBROSIA_GUEST_ABI input_device_callback(std::uint64_t, void *, void *, std::uint64_t, std::uint32_t,
                                              std::uint32_t) {
    ++input_callback_calls;
}

void AMBROSIA_GUEST_ABI input_reading_callback(std::uint64_t, void *, void *, bool) {
    ++input_callback_calls;
}

void AMBROSIA_GUEST_ABI input_guide_callback(std::uint64_t, void *, void *, std::uint64_t, bool) {
    ++input_callback_calls;
}

void game_input_checks() {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    Runtime independent;
    Scope scope(runtime);
    using Create = std::int32_t(AMBROSIA_GUEST_ABI *)(void *);
    const auto address = runtime.resolve(service_symbol("GameInputCreate", "gameinput.dll"));
    auto ordinal = service_symbol("", "gameinput.dll");
    ordinal.by_ordinal = true;
    ordinal.ordinal = 1;
    const auto create = reinterpret_cast<Create>(address);
    check(address && runtime.resolve(ordinal) == address &&
              runtime.resolve(service_symbol("GameInputCreate", "xgameruntime.dll")) == address,
          "GameInputCreate name, confirmed ordinal1 and runtime alias share one typed native factory");
    void *object = nullptr;
    const auto created = create(&object);
    check(created == 0 && object && runtime.game_input().references() == 2 &&
              independent.game_input().references() == 1 && create(nullptr) == pointer_error,
          "the input factory owns a counted per-runtime identity and rejects missing output without claiming "
          "any devices");
    const auto *table = *static_cast<const std::uintptr_t *const *>(object);
    using Query = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, const void *, void *);
    using Release = std::uint32_t(AMBROSIA_GUEST_ABI *)(void *);
    using Timestamp = std::uint64_t(AMBROSIA_GUEST_ABI *)(void *);
    using Reading = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, std::uint32_t, void *, void *);
    using DeviceRoutine = decltype(&input_device_callback);
    using RegisterDevice = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, void *, std::uint32_t, std::uint32_t,
                                                              std::int32_t, void *, DeviceRoutine, void *);
    using RegisterReading = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, void *, std::uint32_t, float, void *,
                                                               decltype(&input_reading_callback), void *);
    using RegisterGuide =
        std::int32_t(AMBROSIA_GUEST_ABI *)(void *, void *, void *, decltype(&input_guide_callback), void *);
    using Stop = void(AMBROSIA_GUEST_ABI *)(void *, std::uint64_t);
    using Remove = bool(AMBROSIA_GUEST_ABI *)(void *, std::uint64_t, std::uint64_t);
    using Focus = void(AMBROSIA_GUEST_ABI *)(void *, std::uint32_t);
    const auto query = reinterpret_cast<Query>(table[0]);
    const auto release = reinterpret_cast<Release>(table[2]);
    const auto timestamp = reinterpret_cast<Timestamp>(table[3]);
    const auto reading = reinterpret_cast<Reading>(table[4]);
    const auto subscribe = reinterpret_cast<RegisterDevice>(table[9]);
    const auto register_reading = reinterpret_cast<RegisterReading>(table[8]);
    const auto register_guide = reinterpret_cast<RegisterGuide>(table[10]);
    const auto stop = reinterpret_cast<Stop>(table[12]);
    const auto remove = reinterpret_cast<Remove>(table[13]);
    const auto focus = reinterpret_cast<Focus>(table[21]);
    void *unknown = nullptr;
    const auto unknown_status = query(object, &unknown_interface_id, &unknown);
    const auto released_unknown = release(unknown);
    const Guid absent{};
    unknown = reinterpret_cast<void *>(1);
    const auto rejected = query(object, &absent, &unknown);
    check(
        unknown_status == 0 && released_unknown == 2 && rejected == no_interface && !unknown,
        "input IUnknown preserves counted identity while unknown interface revisions clear output and fail");
    const auto saved_error = errno;
    errno = EDOM;
    crt_error() = 61;
    set_last_error(91);
    const auto before = static_cast<std::uint64_t>(host::performance_counter()) / 1000;
    const auto first = timestamp(object);
    const auto second = timestamp(object);
    const auto after = static_cast<std::uint64_t>(host::performance_counter()) / 1000;
    check(first >= before && second >= first && second <= after && errno == EDOM && crt_error() == 61 &&
              get_last_error() == 91,
          "input timestamps use actual monotonic microseconds, not milliseconds or nanoseconds, and preserve "
          "caller errors");
    std::array<std::byte, 16> guarded;
    guarded.fill(std::byte{0xcc});
    const auto no_reading = reading(object, 0x40000, nullptr, guarded.data() + 3);
    std::uintptr_t output = 99;
    std::memcpy(&output, guarded.data() + 3, sizeof(output));
    const auto absent_output = output;
    const auto foreign = reading(object, 0x40000, reinterpret_cast<void *>(1), &output);
    check(no_reading == input_reading_not_found && !absent_output && !output &&
              foreign == input_device_not_found && guarded[2] == std::byte{0xcc} &&
              guarded[11] == std::byte{0xcc} && reading(object, 0, nullptr, nullptr) == pointer_error,
          "an empty device collection clears exactly one reading pointer and reports missing readings or "
          "foreign devices without inventing neutral input");
    input_callback_calls = 0;
    int context = 7;
    std::array<std::uint64_t, 3> tokens{};
    bool modes = true;
    for (std::int32_t mode = 0; mode < 3; ++mode) {
        guarded.fill(std::byte{0xcc});
        const auto status = subscribe(object, nullptr, 0x40000, 3, mode, &context, &input_device_callback,
                                      guarded.data() + 1);
        std::memcpy(&tokens[mode], guarded.data() + 1, sizeof(tokens[mode]));
        const auto entry = runtime.game_input().subscription(tokens[mode]);
        modes = modes && status == 0 && tokens[mode] && entry && entry->kind == InputCallbackKind::device &&
                entry->context == &context &&
                entry->routine == reinterpret_cast<std::uintptr_t>(&input_device_callback) &&
                entry->input_kinds == 0x40000 && entry->status_filter == 3 && entry->enumeration == mode &&
                entry->enabled && guarded[0] == std::byte{0xcc} && guarded[9] == std::byte{0xcc};
    }
    check(modes && tokens[0] < tokens[1] && tokens[1] < tokens[2] && !input_callback_calls &&
              !independent.game_input().subscriptions(),
          "all enumeration modes retain exact device callback/filter/context/token records with no "
          "fabricated device callbacks in an empty provider");
    std::uint64_t reading_token = 0, guide_token = 0;
    const auto reading_status =
        register_reading(object, nullptr, 0x40000, 0.25f, &context, &input_reading_callback, &reading_token);
    const auto guide_status = register_guide(object, nullptr, &context, &input_guide_callback, &guide_token);
    const auto recorded = runtime.game_input().subscription(reading_token);
    check(reading_status == 0 && guide_status == 0 && reading_token && guide_token && recorded &&
              recorded->kind == InputCallbackKind::reading && recorded->analog_threshold == 0.25f &&
              recorded->context == &context &&
              recorded->routine == reinterpret_cast<std::uintptr_t>(&input_reading_callback),
          "reading callback Microsoft ABI preserves its floating register argument and stack "
          "context/routine/output while guide callbacks share the owned registry");
    std::uint64_t invalid_token = 99;
    const auto invalid_mode =
        subscribe(object, nullptr, 0, 0, 3, nullptr, &input_device_callback, &invalid_token);
    const auto no_routine = subscribe(object, nullptr, 0, 0, 0, nullptr, nullptr, &invalid_token);
    const auto foreign_device = subscribe(object, reinterpret_cast<void *>(1), 0, 0, 0, nullptr,
                                          &input_device_callback, &invalid_token);
    const auto no_output = subscribe(object, nullptr, 0, 0, 0, nullptr, &input_device_callback, nullptr);
    const auto bad_threshold = register_reading(object, nullptr, 0, std::numeric_limits<float>::quiet_NaN(),
                                                nullptr, &input_reading_callback, &invalid_token);
    check(invalid_mode == gdk_invalid_argument && no_routine == pointer_error &&
              foreign_device == input_device_not_found && no_output == pointer_error &&
              bad_threshold == gdk_invalid_argument && !invalid_token &&
              runtime.game_input().subscriptions() == 5,
          "invalid modes, callbacks, output, device pointers and NaN thresholds cannot publish callbacks or "
          "consume directory entries");
    stop(object, tokens[0]);
    const auto stopped = runtime.game_input().subscription(tokens[0]);
    const auto removed = remove(object, tokens[0], 0);
    const auto repeated = remove(object, tokens[0], ~std::uint64_t{0});
    check(stopped && !stopped->enabled && removed && repeated &&
              !runtime.game_input().subscription(tokens[0]),
          "StopCallback disables its retained entry and timeout variants remove immediately with no active "
          "callbacks");
    bool cleaned = true;
    for (const auto token : {tokens[1], tokens[2], reading_token, guide_token}) {
        cleaned = remove(object, token, 0) && cleaned;
    }
    std::vector<std::uint64_t> bounded;
    for (unsigned index = 0; index < GameInput::callback_limit; ++index) {
        std::uint64_t token = 0;
        const auto status =
            subscribe(object, nullptr, 0x40000, 1, 0, &context, &input_device_callback, &token);
        cleaned = cleaned && status == 0 && token > guide_token;
        bounded.push_back(token);
    }
    invalid_token = 99;
    const auto exhausted =
        subscribe(object, nullptr, 0, 0, 0, &context, &input_device_callback, &invalid_token);
    check(cleaned && exhausted == gdk_out_of_memory && !invalid_token &&
              runtime.game_input().subscriptions() == GameInput::callback_limit,
          "input callbacks have a bounded directory with nonreused IDs and zero output on exhaustion");
    for (const auto token : bounded) {
        (void)remove(object, token, 0);
    }
    focus(object, 0);
    check(!runtime.game_input().subscriptions() && !input_callback_calls && errno == EDOM &&
              crt_error() == 61 && get_last_error() == 91,
          "empty-provider default focus and full cleanup preserve caller errors without native device/window "
          "access or phantom callbacks");
    auto bytes = fixture();
    text(bytes, 0x440, "gameinput.dll");
    put<std::uint64_t>(bytes, 0x460, 0x8000000000000001ull);
    const unsigned char code[] = {0x48, 0x83, 0xec, 0x28, 0x48, 0x8d, 0x0d, 0,    0,    0,    0,
                                  0xff, 0x15, 0,    0,    0,    0,    0x48, 0x83, 0xc4, 0x28, 0xc3};
    std::memcpy(bytes.data() + 0x200, code, sizeof(code));
    put<std::uint32_t>(bytes, 0x207, 0x21f0 - 0x100b);
    put<std::uint32_t>(bytes, 0x20d, 0x2080 - 0x1011);
    auto image = pe::Image::load(bytes, [&](const auto &symbol) { return runtime.resolve(symbol); });
    const auto guest_result = image.run_bootstrap();
    void *guest_object = nullptr;
    std::memcpy(&guest_object, image.base() + 0x21f0, sizeof(guest_object));
    check(guest_result == 0 && guest_object == object && runtime.game_input().references() == 3 &&
              release(guest_object) == 2 && release(object) == 1,
          "original PE ordinal1 factory call writes the real shared input identity and balances all acquired "
          "references");
    errno = saved_error;
}
