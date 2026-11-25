void registry_checks() {
    using namespace ambrosia::services;
    const auto rejects_name = [](auto operation) {
        try {
            operation();
        } catch (const std::invalid_argument &) {
            return true;
        }
        return false;
    };
    constexpr std::uintptr_t machine = 0x80000002u;
    constexpr std::uintptr_t user = 0x80000001u;
    const auto ids = std::make_shared<HandleIds>();
    RegistryKeys keys(ids);
    KernelObjects objects(4096, ids);
    check(keys.open(machine, u"Software\\Absent", true).error == 2 &&
              keys.open(machine, u"Software\\Absent", true).error == 2,
          "empty registry reports actual absence and repeated open does not create a key");
    check(keys.define_key(machine, u"Software\\Ambrosia\\Display") == 0 &&
              keys.define_key(machine, u"SOFTWARE\\AMBROSIA\\DISPLAY") == 0,
          "explicit configuration populates case-insensitive registry paths idempotently");
    const auto software = keys.open(machine, u"software", true);
    const auto display = keys.open(software.handle, u"AMBROSIA\\display", true);
    check(software.handle && !software.error && display.handle && !display.error,
          "configured registry supports both intermediate and relative nested key lookup");
    check(keys.open(user, u"Software\\Ambrosia\\Display", true).error == 2,
          "registry roots own separate namespaces with no fabricated host defaults");
    check(keys.open(display.handle, u"", true).handle == display.handle,
          "legacy empty-subkey lookup returns the original non-predefined handle");
    const auto duplicate = keys.open(display.handle, u"", false);
    check(duplicate.handle && duplicate.handle != display.handle && !duplicate.error,
          "extended empty-subkey lookup duplicates an ordinary key handle");
    check(keys.close(display.handle) == 0 && keys.close(display.handle) == 6 &&
              keys.open(display.handle, u"", false).error == 6 && keys.owns(duplicate.handle),
          "closing a key invalidates only that owned handle and retains its other alias");
    check(keys.close(duplicate.handle) == 0 &&
              keys.open(machine, u"Software\\Ambrosia\\Display", true).handle != 0,
          "closing all opened aliases does not delete configured registry keys");
    const auto sign_extended_machine =
        static_cast<std::uintptr_t>(static_cast<std::intptr_t>(static_cast<std::int32_t>(machine)));
    check(keys.open(sign_extended_machine, u"", false).handle == sign_extended_machine &&
              keys.open(machine, u"", true).handle == machine && keys.close(machine) == 0 &&
              keys.owns(machine),
          "predefined roots accept signed ABI constants and survive empty opens and close");
    const auto event = objects.create_event(false, false);
    check(event.handle && !keys.owns(event.handle) && keys.close(event.handle) == 6 &&
              keys.open(event.handle, u"", true).error == 6 && objects.close(software.handle) == 6 &&
              objects.wait(event.handle, 0).result == wait_timeout,
          "shared opaque ID allocation prevents event/registry type confusion");
    check(keys.open(0, u"", true).error == 6 && keys.open(0x80000004u, u"", true).error == 6 &&
              keys.define_key(software.handle, u"child") == 6,
          "invalid and unsupported predefined handles cannot manufacture registry ownership");
    check(rejects_name([&] { keys.open(machine, u"Clé", true); }) &&
              rejects_name([&] { keys.open(machine, u"\\absolute", true); }) &&
              rejects_name([&] { keys.open(machine, u"double\\\\separator", true); }),
          "unsupported Unicode casing and ambiguous registry separators fail explicitly");
    check(rejects_name([&] { keys.define_key(machine, std::u16string(256, u'x')); }),
          "registry component limit is checked before configuration insertion");
    RegistryKeys limited(std::make_shared<HandleIds>(), 1);
    limited.define_key(machine, u"configured");
    const auto old = limited.open(machine, u"configured", true);
    check(old.handle && limited.open(machine, u"configured", true).error == 8 &&
              limited.close(old.handle) == 0,
          "registry handle exhaustion preserves the existing configured key and handle");
    const auto newer = limited.open(machine, u"configured", true);
    check(newer.handle && newer.handle != old.handle && !limited.owns(old.handle),
          "new registry handles never resurrect a closed stale ID");
    RegistryKeys bounded;
    bool filled = true;
    for (unsigned index = 0; index < 4091; ++index) {
        const auto text = std::to_string(index);
        filled = filled && bounded.define_key(machine, std::u16string(text.begin(), text.end())) == 0;
    }
    check(filled && bounded.define_key(machine, u"one-too-many") == 8 &&
              bounded.open(machine, u"one-too-many", true).error == 2,
          "registry key budget rejects excess configuration without a partial insertion");

    Runtime runtime;
    Scope scope(runtime);
    runtime.registry_keys().define_key(machine, u"Software\\Ambrosia");
    using OpenW = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, const char16_t *, void **);
    using OpenA = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, const char *, void **);
    using OpenEx =
        std::int32_t(AMBROSIA_GUEST_ABI *)(void *, const char16_t *, std::uint32_t, std::uint32_t, void **);
    using Close = std::int32_t(AMBROSIA_GUEST_ABI *)(void *);
    const auto open_w =
        reinterpret_cast<OpenW>(runtime.resolve(service_symbol("RegOpenKeyW", "advapi32.dll")));
    const auto open_a =
        reinterpret_cast<OpenA>(runtime.resolve(service_symbol("RegOpenKeyA", "advapi32.dll")));
    const auto open_ex = reinterpret_cast<OpenEx>(
        runtime.resolve(service_symbol("RegOpenKeyExW", "api-ms-win-core-registry-l1-1-0.dll")));
    const auto close =
        reinterpret_cast<Close>(runtime.resolve(service_symbol("RegCloseKey", "advapi32.dll")));
    const auto close_handle =
        reinterpret_cast<Close>(runtime.resolve(service_symbol("CloseHandle", "kernel32.dll")));
    auto *root = reinterpret_cast<void *>(sign_extended_machine);
    void *handle = nullptr;
    set_last_error(91);
    crt_error() = 23;
    check(open_w(root, u"SOFTWARE\\Ambrosia", &handle) == 0 && handle && get_last_error() == 91 &&
              crt_error() == 23,
          "registry Microsoft ABI resolves owned state and returns LSTATUS without changing thread errors");
    void *narrow = nullptr;
    check(open_a(root, "software\\ambrosia", &narrow) == 0 && narrow && narrow != handle,
          "narrow and wide registry aliases open the same configured key with separate ownership");
    void *missing = handle;
    check(open_w(root, u"Software\\Missing", &missing) == 2 && !missing && get_last_error() == 91,
          "missing registry ABI lookup returns a direct status without fabricating a handle");
    void *same = nullptr;
    void *extended = nullptr;
    check(open_w(handle, nullptr, &same) == 0 && same == handle &&
              open_ex(handle, u"", 0, 0x20119, &extended) == 0 && extended && extended != handle,
          "legacy and extended ABI preserve their distinct null/empty subkey ownership contracts");
    check(close_handle(handle) == 1 &&
              runtime.registry_keys().owns(reinterpret_cast<std::uintptr_t>(handle)) &&
              get_last_error() == 91 && close(handle) == 0 && close(handle) == 6,
          "CloseHandle does not close a registry key; RegCloseKey alone consumes its ownership");
    check(open_w(root, u"Software", nullptr) == 87 && get_last_error() == 91 && crt_error() == 23,
          "null registry output fails invalid-parameter while preserving independent error channels");
    check(close(narrow) == 0 && close(extended) == 0 && close(root) == 0 &&
              open_ex(root, nullptr, 0, 0x20019, &same) == 0 && same == root,
          "owned alias cleanup leaves the predefined root usable through the extended API");
}
