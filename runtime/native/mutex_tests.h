void mutex_checks() {
    using namespace ambrosia;
    using namespace services;
    KernelObjects objects;
    guest::ThreadContext first;
    guest::ThreadContext second;
    std::uintptr_t handle = 0;
    std::uintptr_t alias = 0;
    {
        auto activation = first.activate();
        const auto created = objects.create_mutex(true, u"owned-mutex");
        handle = created.handle;
        check(handle && !created.error && objects.wait(handle, 0).result == wait_object_0 &&
                  objects.wait(handle, 0).result == wait_object_0 && !objects.release_mutex(handle) &&
                  !objects.release_mutex(handle),
              "initial mutex ownership supports recursive acquisition and balanced partial releases");
    }
    {
        auto activation = second.activate();
        const auto existing = objects.create_mutex(true, u"owned-mutex");
        alias = existing.handle;
        check(alias && alias != handle && existing.error == 183 &&
                  objects.wait(alias, 0).result == wait_timeout &&
                  objects.wait(alias, 1).result == wait_timeout && objects.release_mutex(alias) == 288,
              "existing mutex creation ignores initial-owner and distinct guest threads cannot steal "
              "ownership");
    }
    {
        auto activation = first.activate();
        check(!objects.release_mutex(handle) && objects.release_mutex(handle) == 288,
              "only the final recursive mutex release makes it available; excess releases fail");
    }
    {
        auto activation = second.activate();
        check(objects.wait(alias, 0).result == wait_object_0 && !objects.release_mutex(alias),
              "released mutex ownership transfers to another guest context on the same host thread");
    }
    check(objects.create_event(false, false, u"owned-mutex").error == 6 &&
              objects.create_semaphore(0, 1, u"owned-mutex").error == 6 && objects.set_event(handle) == 6 &&
              objects.release_semaphore(handle, 1).error == 6,
          "mutex names share the typed object namespace without becoming events or semaphores");
    const auto event = objects.create_event(false, false, u"event-name");
    check(event.handle && objects.create_mutex(false, u"event-name").error == 6 &&
              objects.release_mutex(event.handle) == 6,
          "wrong object types cannot create or release mutex ownership");
    check(!objects.close(handle) && !objects.close(alias) && objects.release_mutex(alias) == 6,
          "mutex handles close independently and stale IDs cannot release a recreated object");
    const auto recreated = objects.create_mutex(false, u"owned-mutex");
    check(recreated.handle && !recreated.error && recreated.handle != handle && recreated.handle != alias,
          "last-close removes the original named mutex identity");
    {
        auto activation = first.activate();
        bool balanced = true;
        for (unsigned i = 0; i < 200; ++i) {
            balanced &= objects.wait(recreated.handle, 0).result == wait_object_0;
            balanced &= !objects.release_mutex(recreated.handle);
        }
        check(balanced,
              "repeated mutex acquisitions reuse one cleanup hook instead of exhausting thread callbacks");
    }

    auto exiting = std::make_unique<guest::ThreadContext>();
    std::vector<std::uintptr_t> abandoned;
    {
        auto activation = exiting->activate();
        for (unsigned i = 0; i < 80; ++i) {
            abandoned.push_back(objects.create_mutex(true).handle);
        }
    }
    exiting.reset();
    {
        auto activation = second.activate();
        bool transferred = true;
        for (const auto owned : abandoned) {
            transferred &= owned && objects.wait(owned, 0).result == wait_abandoned;
            transferred &= objects.wait(owned, 0).result == wait_object_0;
            transferred &= !objects.release_mutex(owned) && !objects.release_mutex(owned);
            transferred &= !objects.close(owned);
        }
        check(transferred, "guest-thread exit abandons every owned mutex; the first waiter receives "
                           "ownership and status 128 once");
    }
    auto late_context = std::make_unique<guest::ThreadContext>();
    {
        KernelObjects temporary;
        auto activation = late_context->activate();
        check(temporary.create_mutex(true).handle != 0,
              "mutex directory may be destroyed before a guest context cleanup hook");
    }
    late_context.reset();
    KernelObjects limited(1);
    const auto only = limited.create_mutex(false);
    check(only.handle && limited.create_mutex(false).error == 8 && !limited.close(only.handle) &&
              limited.create_mutex(false).handle != only.handle,
          "mutex creation shares bounded opaque handle allocation without resurrecting closed IDs");

    Runtime runtime;
    Scope scope(runtime);
    auto activation = first.activate();
    using CreateA = void *(AMBROSIA_GUEST_ABI *)(void *, std::int32_t, const char *);
    using CreateW = void *(AMBROSIA_GUEST_ABI *)(void *, std::int32_t, const char16_t *);
    using Release = std::int32_t(AMBROSIA_GUEST_ABI *)(void *);
    const auto address = runtime.resolve(service_symbol("CreateMutexA", "kernel32.dll"));
    const auto create_a = reinterpret_cast<CreateA>(address);
    const auto create_w =
        reinterpret_cast<CreateW>(runtime.resolve(service_symbol("CreateMutexW", "kernel32.dll")));
    const auto release =
        reinterpret_cast<Release>(runtime.resolve(service_symbol("ReleaseMutex", "kernel32.dll")));
    check(address && address == runtime.resolve(service_symbol("CreateMutexA", "kernelbase.dll")) &&
              address == runtime.resolve(service_symbol("CreateMutexA", "api-ms-win-core-synch-l1-1-0.dll")),
          "mutex API-set and kernel aliases share the callable Microsoft ABI facade");
    crt_error() = 17;
    const auto saved_error = errno;
    errno = E2BIG;
    auto *native_handle = create_a(nullptr, 1, "Local\\native-mutex");
    const auto created_error = get_last_error();
    auto *native_alias = create_w(nullptr, 1, u"native-mutex");
    check(native_handle && native_alias && native_handle != native_alias && !created_error &&
              get_last_error() == 183,
          "narrow and UTF-16 CreateMutex share a process-local name and existing-object status");
    set_last_error(89);
    check(release(native_alias) && get_last_error() == 89 && !release(native_handle) &&
              get_last_error() == 288 && errno == E2BIG && crt_error() == 17,
          "ReleaseMutex preserves error state on success and reports non-ownership without changing CRT "
          "errors");
    const std::string long_name(260, 'x');
    check(!create_a(nullptr, 0, long_name.c_str()) && get_last_error() == 206 &&
              !create_w(nullptr, 0, u"invalid\\name") && get_last_error() == 123,
          "mutex names retain bounded process-local namespace validation");

    auto caller = fixture();
    text(caller, 0x440, "kernel32.dll");
    text(caller, 0x4a2, "CreateMutexA");
    // sub rsp,28h; attributes=null; initial_owner=0; name=null; call IAT; return handle.
    constexpr unsigned char code[] = {0x48, 0x83, 0xec, 0x28, 0x31, 0xc9, 0x31, 0xd2, 0x45, 0x31, 0xc0,
                                      0xff, 0x15, 0,    0,    0,    0,    0x48, 0x83, 0xc4, 0x28, 0xc3};
    std::memcpy(caller.data() + 0x200, code, sizeof(code));
    put<std::uint32_t>(caller, 0x20d, 0x2080 - (0x1000 + 17));
    auto image = pe::Image::load(caller, [&](const auto &symbol) { return runtime.resolve(symbol); });
    const auto imported = image.run_bootstrap();
    check(imported && runtime.objects().wait(imported, 0).result == wait_object_0 &&
              !runtime.objects().release_mutex(imported) && !runtime.objects().close(imported),
          "an original PE CreateMutexA import returns a genuine owned waitable handle");
    runtime.objects().close(reinterpret_cast<std::uintptr_t>(native_handle));
    runtime.objects().close(reinterpret_cast<std::uintptr_t>(native_alias));
    errno = saved_error;
}
