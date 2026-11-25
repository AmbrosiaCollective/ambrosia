void locale_lock_checks() {
    using namespace ambrosia::services;
    Runtime runtime;
    Runtime independent;
    check(runtime_rejected([&] { runtime.unlock_locales(); }),
          "locale unlock without ownership fails explicitly");
    using Operation = void(AMBROSIA_GUEST_ABI *)();
    const auto acquire = reinterpret_cast<Operation>(
        runtime.resolve(service_symbol("_lock_locales", "api-ms-win-crt-locale-l1-1-0.dll")));
    const auto release = reinterpret_cast<Operation>(
        runtime.resolve(service_symbol("_unlock_locales", "api-ms-win-crt-locale-l1-1-0.dll")));
    Scope scope(runtime);
    set_last_error(71);
    crt_error() = 19;
    acquire();
    acquire();
    release();
    // Releasing through the core verifies that the ABI adapter acquired real
    // owned state, rather than merely returning from a no-op wrapper.
    runtime.unlock_locales();
    check(runtime_rejected([&] { runtime.unlock_locales(); }),
          "recursive locale ABI locking balances two real ownership levels");
    check(get_last_error() == 71 && crt_error() == 19,
          "locale lock/unlock preserve Win32 and CRT error values");
    acquire();
    {
        Scope nested(independent);
        check(runtime_rejected([&] { independent.unlock_locales(); }),
              "independent runtime does not inherit locale lock ownership");
        acquire();
        release();
    }
    runtime.unlock_locales();
    check(runtime_rejected([&] { runtime.unlock_locales(); }),
          "nested locale runtime restores the original lock boundary");
}
