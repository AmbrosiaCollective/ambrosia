bool AMBROSIA_GUEST_ABI unexpected_content_package(void *context, const void *) {
    ++*static_cast<unsigned *>(context);
    return true;
}

void package_enumeration_checks() {
    using namespace ambrosia;
    using namespace services;
    constexpr auto unsupported = static_cast<std::int32_t>(0x80070032u);
    Runtime unbound;
    check(unbound.gdk_packages().enumerate_packages(1, 0, true) == unsupported &&
              runtime_rejected([&] { unbound.gdk_packages().bind_current_game_catalog(); }),
          "an unconfigured process cannot report a successful empty installed-package catalog");
    PackageMonitorFixture fixture("package-enumeration");
    auto &packages = fixture.runtime.gdk_packages();
    check(packages.enumerate_packages(1, 0, true) == unsupported,
          "matching identity and size evidence alone do not silently configure a package catalog");
    auto bytes = tls_fixture();
    put<std::uint32_t>(bytes, 0x98 + 120, 0);
    put<std::uint32_t>(bytes, 0x98 + 124, 0);
    jump_to_host(bytes, 0x300, reinterpret_cast<std::uintptr_t>(&worker_module_callback));
    jump_to_host(bytes, 0x380, reinterpret_cast<std::uintptr_t>(&unexpected_content_package));
    pe::ModuleGraph graph;
    graph.add("enumeration.exe", bytes);
    graph.load("enumeration.exe");
    check(graph.statistics().executable_root,
          "the original catalog fixture maps an executable application rather than a content package");
    packages.bind_current_game_catalog();
    check(packages.enumerate_packages(1, 0, true) == 0 &&
              runtime_rejected([&] { packages.bind_current_game_catalog(); }),
          "an explicitly bound current-game catalog contains no separately registered DLC and binds once");
    check(
        packages.enumerate_packages(0, 0, true) == unsupported &&
            packages.enumerate_packages(2, 0, true) == unsupported &&
            packages.enumerate_packages(1, 1, true) == unsupported &&
            packages.enumerate_packages(1, 2, true) == unsupported,
        "game entries, publisher content and broader scopes remain unsupported without their catalog model");
    check(packages.enumerate_packages(3, 0, true) == gdk_invalid_argument &&
              packages.enumerate_packages(1, 3, true) == gdk_invalid_argument &&
              packages.enumerate_packages(1, 0, false) == gdk_invalid_argument,
          "invalid kind/scope values or absent callbacks cannot silently enumerate a different scope");
    Scope scope(fixture.runtime, &graph);
    auto process = graph.start();
    const auto *table = *static_cast<const std::uintptr_t *const *>(fixture.object);
    using Routine = bool(AMBROSIA_GUEST_ABI *)(void *, const void *);
    using Enumerate =
        std::int32_t(AMBROSIA_GUEST_ABI *)(void *, std::uint32_t, std::uint32_t, void *, Routine);
    const auto enumerate = reinterpret_cast<Enumerate>(table[39]);
    const auto callback = reinterpret_cast<Routine>(graph.loaded_module() + 0x1180);
    unsigned entries = 0;
    const auto saved_errno = errno;
    process.with_guest_context([&] {
        errno = EDOM;
        set_last_error(91);
        crt_error() = 61;
        const auto result = enumerate(fixture.object, 1, 0, &entries, callback);
        check(result == 0 && entries == 0 && errno == EDOM && get_last_error() == 91 && crt_error() == 61,
              "the observed console ABI enumerates zero DLC without invoking a callback or changing errors");
        check(enumerate(fixture.object, 1, 0, &entries, nullptr) == gdk_invalid_argument && entries == 0 &&
                  enumerate(fixture.object, 1, 0, &entries, &unexpected_content_package) == unsupported,
              "null and unmapped callbacks fail before any catalog result or guest execution");
    });
    packages.quiesce();
    check(packages.enumerate_packages(1, 0, true) == unsupported,
          "a quiesced catalog cannot accept new enumeration requests during teardown");
    process.close();
    errno = saved_errno;
}
