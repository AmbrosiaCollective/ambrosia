void runtime_interface_checks() {
    using namespace ambrosia::services;
    const Guid identity{1, 2, 3, {4, 5, 6, 7, 8, 9, 10, 11}};
    const Guid derived{2, 3, 4, {5, 6, 7, 8, 9, 10, 11, 12}};
    const Guid absent{};
    const std::array ids{derived};
    const std::array<std::uintptr_t, 3> table{reinterpret_cast<std::uintptr_t>(&interface_query),
                                              reinterpret_cast<std::uintptr_t>(&interface_add_reference),
                                              reinterpret_cast<std::uintptr_t>(&interface_release)};
    RuntimeInterface object(table.data(), identity, ids);
    void *result = reinterpret_cast<void *>(1);
    check(
        interface_query(&object, &absent, &result) == no_interface && !result && object.references == 1 &&
            interface_query(&object, &identity, nullptr) == pointer_error &&
            interface_query(nullptr, &identity, &result) == pointer_error && !result,
        "native interface queries reject unknown IDs and missing pointers without publishing a fake object");
    check(interface_query(&object, &identity, &result) == 0 && result == &object && object.references == 2 &&
              interface_add_reference(result) == 3 && interface_release(result) == 2 &&
              interface_release(result) == 1 && interface_release(result) == 1,
          "native interface references use Windows ULONG widths and retain the runtime owner pin");
    check(interface_query(&object, &unknown_interface_id, &result) == 0 && result == &object &&
              interface_release(result) == 1 && interface_query(&object, &derived, &result) == 0 &&
              interface_release(result) == 1,
          "IUnknown, class and inherited interface IDs resolve to one retained identity");
    std::array<std::byte, 20> unaligned_id;
    std::memcpy(unaligned_id.data() + 1, &identity, sizeof(identity));
    std::array<std::byte, 16> output;
    output.fill(std::byte{0xcc});
    const auto code = interface_query(&object, unaligned_id.data() + 1, output.data() + 3);
    void *stored = nullptr;
    std::memcpy(&stored, output.data() + 3, sizeof(stored));
    check(code == 0 && stored == &object && output[2] == std::byte{0xcc} && output[11] == std::byte{0xcc},
          "interface GUID and output pointer copying does not assume guest buffer alignment or overwrite "
          "guards");
    (void)interface_release(stored);
    for (unsigned i = 0; i < 4095; ++i) {
        (void)interface_add_reference(&object);
    }
    check(object.references == 4096,
          "guest interface references stop at the documented runtime resource boundary");
    for (unsigned i = 0; i < 4095; ++i) {
        (void)interface_release(&object);
    }
    check(object.references == 1,
          "balanced interface releases recover the process-owned pin without deleting the object");
}

void gdk_threading_checks() {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    Runtime independent;
    Scope scope(runtime);
    (void)runtime.gdk_bootstrap().initialize(1, 2);
    using Query = std::int32_t(AMBROSIA_GUEST_ABI *)(const void *, const void *, void *);
    const auto query =
        reinterpret_cast<Query>(runtime.resolve(service_symbol("QueryApiImpl", "xgameruntime.dll")));
    void *object = nullptr;
    const auto saved_error = errno;
    errno = E2BIG;
    crt_error() = 19;
    set_last_error(87);
    check(query(&threading_class_id, &threading_class_id, &object) == 0 && object &&
              runtime.gdk_threading().references() == 2 && independent.gdk_threading().references() == 1,
          "QueryApiImpl returns the owned XThreadingImpl object with a counted reference independent of "
          "other runtimes");
    const auto *vtable = *static_cast<const std::uintptr_t *const *>(object);
    using QueryInterface = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, const void *, void *);
    using Reference = std::uint32_t(AMBROSIA_GUEST_ABI *)(void *);
    using SetSensitive = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, bool);
    using IsSensitive = bool(AMBROSIA_GUEST_ABI *)(void *);
    using Assert = void(AMBROSIA_GUEST_ABI *)(void *);
    const auto qi = reinterpret_cast<QueryInterface>(vtable[0]);
    const auto add = reinterpret_cast<Reference>(vtable[1]);
    const auto release = reinterpret_cast<Reference>(vtable[2]);
    const auto set = reinterpret_cast<SetSensitive>(vtable[27]);
    const auto asserted = reinterpret_cast<Assert>(vtable[29]);
    const auto get = reinterpret_cast<IsSensitive>(vtable[30]);
    void *alias = nullptr;
    check(qi(object, &unknown_interface_id, &alias) == 0 && alias == object && add(object) == 4 &&
              release(object) == 3 && release(alias) == 2,
          "the actual Windows vtable prefix dispatches typed IUnknown identity/AddRef/Release methods");
    const Guid unknown{};
    alias = reinterpret_cast<void *>(1);
    check(qi(object, &unknown, &alias) == no_interface && !alias && runtime.gdk_threading().references() == 2,
          "an unsupported threading interface revision does not return a guessed vtable");
    guest::ThreadContext first;
    guest::ThreadContext second;
    {
        auto activation = first.activate();
        const auto initial = get(object);
        const auto enabled = set(object, true);
        const auto sensitive = get(object);
        check(!initial && enabled == 0 && sensitive,
              "GDK thread sensitivity is mutable guest-thread state rather than a constant result");
        {
            auto nested = second.activate();
            const auto nested_initial = get(object);
            const auto nested_enabled = set(object, true);
            const auto nested_sensitive = get(object);
            check(!nested_initial && nested_enabled == 0 && nested_sensitive,
                  "another guest context starts with independent sensitivity on the same native host thread");
            set(object, false);
            asserted(object);
        }
        const auto restored = get(object);
        const auto cleared = set(object, false);
        const auto after_clear = get(object);
        check(restored && cleared == 0 && !after_clear,
              "restoring a guest context restores its own sensitivity; clearing satisfies the blocking "
              "assertion");
        asserted(object);
    }
    check(errno == E2BIG && crt_error() == 19 && get_last_error() == 87,
          "interface lifetime and thread properties preserve native errno and guest error states");

    auto caller = fixture();
    text(caller, 0x440, "xgameruntime.dll");
    text(caller, 0x4a2, "QueryApiImpl");
    std::memcpy(caller.data() + 0x540, &threading_class_id, sizeof(threading_class_id));
    // Query the interface, load its vtable, and call slot30. All argument and
    // bool-return handling is original PE code, not a host-side shortcut.
    constexpr unsigned char code[] = {0x48, 0x83, 0xec, 0x28, 0x48, 0x8d, 0x0d, 0,    0,    0,    0,
                                      0x48, 0x8d, 0x15, 0,    0,    0,    0,    0x4c, 0x8d, 0x05, 0,
                                      0,    0,    0,    0xff, 0x15, 0,    0,    0,    0,    0x48, 0x8b,
                                      0x0d, 0,    0,    0,    0,    0x48, 0x8b, 0x01, 0xff, 0x90, 0xf0,
                                      0,    0,    0,    0x0f, 0xb6, 0xc0, 0x48, 0x83, 0xc4, 0x28, 0xc3};
    std::memcpy(caller.data() + 0x200, code, sizeof(code));
    put<std::uint32_t>(caller, 0x207, 0x2140 - (0x1000 + 11));
    put<std::uint32_t>(caller, 0x20e, 0x2140 - (0x1000 + 18));
    put<std::uint32_t>(caller, 0x215, 0x2150 - (0x1000 + 25));
    put<std::uint32_t>(caller, 0x21b, 0x2080 - (0x1000 + 31));
    put<std::uint32_t>(caller, 0x222, 0x2150 - (0x1000 + 38));
    pe::ModuleGraph graph([&](const auto &symbol) { return runtime.resolve(symbol); });
    graph.add("threading.exe", caller);
    graph.load("threading.exe");
    Scope graph_scope(runtime, &graph);
    auto execution = graph.start();
    execution.with_guest_context([&] { set(object, true); });
    const auto result = execution.run_bootstrap();
    void *from_pe = nullptr;
    std::memcpy(&from_pe, reinterpret_cast<const std::byte *>(graph.loaded_module()) + 0x2150,
                sizeof(from_pe));
    check(result == 1 && from_pe == object && runtime.gdk_threading().references() == 3,
          "an original PE queries the live GDK interface and calls its bool-returning vtable slot with real "
          "guest TLS state");
    (void)release(from_pe);
    check(release(object) == 1 && runtime.gdk_threading().references() == 1,
          "host and PE callers release their separate interface references back to the runtime pin");
    errno = saved_error;
}
