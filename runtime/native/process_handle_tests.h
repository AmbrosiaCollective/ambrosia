void current_process_handle_checks() {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    using CurrentProcess = std::uintptr_t(AMBROSIA_GUEST_ABI *)();
    const auto address = runtime.resolve(service_symbol("GetCurrentProcess", "kernel32.dll"));
    const auto current = reinterpret_cast<CurrentProcess>(address);
    const auto expected = ~std::uintptr_t{0};
    bool aliases = true;
    for (const auto *module : {"kernelbase.dll", "api-ms-win-core-processthreads-l1-1-0.dll",
                               "api-ms-win-core-processthreads-l1-1-1.dll"}) {
        aliases = aliases && runtime.resolve(service_symbol("GetCurrentProcess", module)) == address;
    }
    check(address && aliases && current() == expected,
          "GetCurrentProcess contracts return the full-width guest current-process pseudo-handle");
    const auto saved_error = errno;
    errno = EDOM;
    crt_error() = 61;
    set_last_error(91);
    const auto repeated = current();
    check(repeated == expected && errno == EDOM && crt_error() == 61 && get_last_error() == 91,
          "current-process pseudo-handle is stable and does not disturb caller error state");
    const auto closed = runtime.objects().close(repeated);
    check(closed == 6 && current() == expected,
          "a pseudo-handle cannot be closed as an owned kernel object or consume a handle slot");
    auto bytes = fixture();
    text(bytes, 0x440, "kernel32.dll");
    text(bytes, 0x4a2, "GetCurrentProcess");
    auto image = pe::Image::load(bytes, [&](const auto &symbol) { return runtime.resolve(symbol); });
    check(image.run_bootstrap() == expected, "original PE import calls GetCurrentProcess through Microsoft "
                                             "ABI without narrowing the returned handle");
    errno = saved_error;
}
