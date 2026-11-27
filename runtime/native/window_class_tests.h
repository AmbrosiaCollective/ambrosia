void window_class_checks() {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    Runtime independent;
    using Register = std::uint16_t(AMBROSIA_GUEST_ABI *)(const void *);
    const auto address = runtime.resolve(service_symbol("RegisterClassExA", "user32.dll"));
    check(address &&
              address == runtime.resolve(service_symbol("RegisterClassExA",
                                                        "ext-ms-win-rtcore-ntuser-window-ansi-l1-1-0.dll")) &&
              address == runtime.resolve(
                             service_symbol("RegisterClassExA", "ext-ms-win-ntuser-windowclass-l1-1-0.dll")),
          "ANSI window-class aliases share one Windows x64 metadata-registration service");
    if (!address) {
        return;
    }
    const auto register_class = reinterpret_cast<Register>(address);
    auto bytes = fixture();
    put<std::uint32_t>(bytes, 0x98 + 120, 0);
    put<std::uint32_t>(bytes, 0x98 + 124, 0);
    pe::ModuleGraph graph;
    graph.add("class-owner.exe", bytes);
    graph.load("class-owner.exe");
    const auto instance = graph.loaded_module();
    const auto procedure = instance + 0x1000;
    Scope scope(runtime, &graph);
    guest::ThreadContext context;
    auto activation = context.activate();
    std::array<std::byte, 82> packed{};
    auto *input = packed.data() + 1;
    const auto set = [&]<class Value>(std::size_t offset, Value value) {
        std::memcpy(input + offset, &value, sizeof(value));
    };
    std::string name = "OriginalClass";
    std::string menu = "OriginalMenu";
    set(0, std::uint32_t{80});
    set(4, std::uint32_t{3});
    set(8, procedure);
    set(16, std::int32_t{8});
    set(20, std::int32_t{16});
    set(24, instance);
    set(32, std::uint64_t{101});
    set(40, std::uint64_t{102});
    set(48, std::uint64_t{6});
    set(56, reinterpret_cast<std::uintptr_t>(menu.c_str()));
    set(64, reinterpret_cast<std::uintptr_t>(name.c_str()));
    set(72, std::uint64_t{103});
    packed.front() = packed.back() = std::byte{0xcc};
    const auto original_input = packed;
    const auto saved_errno = errno;
    errno = E2BIG;
    crt_error() = 19;
    set_last_error(83);
    const auto atom = register_class(input);
    auto entry = runtime.window_classes().find(atom, instance);
    check(atom >= 0xc000 && entry && packed == original_input && errno == E2BIG && crt_error() == 19 &&
              get_last_error() == 83,
          "packed class registration owns a real atom while preserving input guards and host/guest errors");
    if (!entry) {
        errno = saved_errno;
        return;
    }
    name.assign(name.size(), 'x');
    menu.assign(menu.size(), 'y');
    const auto &owned = entry->description;
    check(owned.name == "OriginalClass" && std::get<std::string>(owned.menu) == "OriginalMenu" &&
              owned.procedure == procedure && owned.instance == instance && owned.style == 3 &&
              owned.icon == 101 && owned.cursor == 102 && owned.background == 6 && owned.small_icon == 103 &&
              owned.window_extra_bytes == 16 && owned.class_extra.size() == 8 &&
              std::all_of(owned.class_extra.begin(), owned.class_extra.end(),
                          [](auto value) { return value == std::byte{0}; }) &&
              owned.modules->module_base_for_address(procedure) == instance,
          "the class copies names, resource metadata and zeroed extra storage while retaining its callback "
          "catalog");
    name = "originalclass";
    set(64, reinterpret_cast<std::uintptr_t>(name.c_str()));
    check(register_class(input) == 0 && get_last_error() == 1410 &&
              runtime.window_classes().find("ORIGINALCLASS", instance) == entry,
          "ASCII case-insensitive duplicate registration fails instead of returning an existing atom as new");
    auto another = owned;
    another.instance = instance + 1;
    const auto same_name_other_module = runtime.window_classes().add(another);
    check(same_name_other_module.atom == atom &&
              runtime.window_classes().find(atom, another.instance)->description.instance ==
                  another.instance &&
              runtime.window_classes().find(atom, instance) == entry &&
              !runtime.window_classes().find(atom, instance + 2),
          "application-local classes share a name atom but remain scoped to their instance");
    another.style |= class_global_style;
    another.instance = instance + 3;
    const auto global = runtime.window_classes().add(another);
    auto global_entry = runtime.window_classes().find(atom, instance + 2);
    check(global.atom == atom && global_entry && global_entry->description.instance == another.instance &&
              runtime.window_classes().find(atom, instance) == entry &&
              runtime.window_classes().add(another).error == 1410,
          "local lookup takes precedence over a global class and duplicate global identity is rejected");
    check(!independent.window_classes().find(atom, instance) &&
              independent.window_classes().add(owned).atom == atom,
          "independent runtime process registries never expose each other's classes");
    set(64, std::uint64_t{atom});
    check(register_class(input) == 0 && get_last_error() == 1410,
          "a registered class atom resolves to its owned name rather than being dereferenced as a pointer");
    set(64, std::uint64_t{7});
    check(register_class(input) == 0 && get_last_error() == error_invalid_parameter &&
              register_class(nullptr) == 0 && get_last_error() == 998,
          "unknown class atoms and missing structures fail with real registration errors");
    set(64, reinterpret_cast<std::uintptr_t>("valid"));
    set(0, std::uint32_t{4});
    check(register_class(input) == 0 && get_last_error() == error_invalid_parameter,
          "a wrong cbSize fails before the rest of a class structure is consumed");
    set(0, std::uint32_t{80});
    set(16, std::int32_t{-1});
    const auto negative_extra = register_class(input);
    set(16, std::int32_t{41});
    const auto oversized_extra = register_class(input);
    set(16, std::int32_t{0});
    set(20, std::int32_t{41});
    check(!negative_extra && !oversized_extra && register_class(input) == 0 &&
              get_last_error() == error_invalid_parameter,
          "negative or oversized class/window extra storage cannot allocate unbounded metadata");
    set(20, std::int32_t{0});
    std::string too_long(257, 'a');
    set(64, reinterpret_cast<std::uintptr_t>(too_long.c_str()));
    const auto oversized_name = register_class(input);
    set(64, reinterpret_cast<std::uintptr_t>(""));
    check(!oversized_name && register_class(input) == 0 && get_last_error() == error_invalid_parameter,
          "empty and over256-byte ASCII class names cannot register");
    set(64, reinterpret_cast<std::uintptr_t>("ResourceClass"));
    set(56, std::uint64_t{9});
    set(8, std::uint64_t{0});
    check(register_class(input) == 0 && get_last_error() == error_invalid_parameter,
          "a missing window procedure cannot create a usable class");
    set(8, procedure);
    const auto resource_atom = register_class(input);
    auto resource = runtime.window_classes().find(resource_atom, instance);
    check(resource && std::get<std::uint16_t>(resource->description.menu) == 9 &&
              resource->description.class_extra.empty(),
          "integer menu resource IDs remain typed metadata without reading low pointer addresses");
    WindowClasses limited;
    bool filled = true;
    for (unsigned index = 0; index < 128; ++index) {
        auto item = owned;
        item.name = "Class" + std::to_string(index);
        filled = filled && limited.add(std::move(item)).atom != 0;
    }
    auto overflow = owned;
    overflow.name = "Overflow";
    check(filled && limited.add(std::move(overflow)).error == error_not_enough_memory,
          "bounded registry capacity fails explicitly after128 real classes");
    // This PE calls RegisterClassExA with a pointer to its own packed structure.
    auto caller = fixture();
    text(caller, 0x440, "user32.dll");
    text(caller, 0x4a2, "RegisterClassExA");
    constexpr unsigned char code[] = {0x48, 0x83, 0xec, 0x28, 0x48, 0x8d, 0x0d, 0,    0,    0,    0,
                                      0xff, 0x15, 0,    0,    0,    0,    0x48, 0x83, 0xc4, 0x28, 0xc3};
    std::memcpy(caller.data() + 0x200, code, sizeof(code));
    put<std::uint32_t>(caller, 0x207, 0x2141 - (0x1000 + 11));
    put<std::uint32_t>(caller, 0x20d, 0x2080 - (0x1000 + 17));
    std::memset(caller.data() + 0x541, 0, 80);
    put<std::uint32_t>(caller, 0x541, 80);
    put<std::uint64_t>(caller, 0x549, procedure);
    put<std::uint64_t>(caller, 0x559, instance);
    put<std::uint64_t>(caller, 0x581, reinterpret_cast<std::uintptr_t>("OriginalPEClass"));
    auto image = pe::Image::load(caller, [&](const auto &symbol) { return runtime.resolve(symbol); });
    const auto returned_atom = static_cast<std::uint16_t>(image.run_bootstrap());
    check(returned_atom >= 0xc000 && runtime.window_classes().find(returned_atom, instance) &&
              runtime.window_classes().find("OriginalPEClass", instance)->atom == returned_atom,
          "an original PE passes packed class attributes through the actual Windows import and receives its "
          "owned atom");
    errno = saved_errno;
}

int window_class_rejection_probe(unsigned mode) {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    using Register = std::uint16_t(AMBROSIA_GUEST_ABI *)(const void *);
    const auto register_class =
        reinterpret_cast<Register>(runtime.resolve(service_symbol("RegisterClassExA", "user32.dll")));
    if (mode == 1) {
        Scope scope(runtime);
        (void)register_class(nullptr);
        return 1;
    }
    auto bytes = fixture();
    put<std::uint32_t>(bytes, 0x98 + 120, 0);
    put<std::uint32_t>(bytes, 0x98 + 124, 0);
    pe::ModuleGraph graph;
    graph.add("class-guard.exe", bytes);
    graph.load("class-guard.exe");
    Scope scope(runtime, &graph);
    guest::ThreadContext context;
    auto activation = context.activate();
    std::array<std::byte, 80> input{};
    const auto set = [&]<class Value>(std::size_t offset, Value value) {
        std::memcpy(input.data() + offset, &value, sizeof(value));
    };
    set(0, std::uint32_t{80});
    set(8, graph.loaded_module() + 0x1000);
    set(24, graph.loaded_module());
    set(64, reinterpret_cast<std::uintptr_t>("ClassGuard"));
    if (mode == 2) {
        set(8, std::uint64_t{1});
    } else if (mode == 3) {
        set(24, std::uint64_t{1});
    } else if (mode == 4) {
        set(64, reinterpret_cast<std::uintptr_t>("\xc3\xa9"));
    } else {
        return 2;
    }
    (void)register_class(input.data());
    return 1;
}
