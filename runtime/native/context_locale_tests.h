void context_locale_checks() {
    using namespace ambrosia::services;
    Runtime runtime;
    Scope scope(runtime);
    auto *global = &runtime.locale();
    check(runtime.configure_thread_locale(1) == -1,
          "thread-locale configuration requires an active guest context");
    using Configure = std::int32_t(AMBROSIA_GUEST_ABI *)(std::int32_t);
    const auto configure = reinterpret_cast<Configure>(
        runtime.resolve(service_symbol("_configthreadlocale", "api-ms-win-crt-locale-l1-1-0.dll")));
    ambrosia::guest::ThreadContext first;
    ambrosia::guest::ThreadContext second;
    {
        auto active = first.activate();
        set_last_error(53);
        crt_error() = 23;
        check(configure(0) == 2 && &runtime.locale() == global,
              "new guest context uses the shared locale with thread mode disabled");
        check(configure(1) == 2 && configure(0) == 1 && &runtime.locale() != global,
              "enabling thread locale returns the old mode and selects owned context storage");
        auto *local = &runtime.locale();
        check(std::string_view(local->select(0, nullptr)) == "C" && local->codepage() == 0 &&
                  local->classification() != global->classification(),
              "thread locale has an independent C-state snapshot and classification table");
        check(get_last_error() == 53 && crt_error() == 23,
              "successful context-locale configuration preserves both error states");
        check(configure(7) == -1 && crt_error() == 22 && configure(0) == 1 && &runtime.locale() == local,
              "unsupported thread-locale flags fail without replacing the enabled mode");
        {
            auto nested = second.activate();
            check(configure(0) == 2 && &runtime.locale() == global,
                  "another guest context does not inherit a sibling's enabled locale mode");
            check(configure(1) == 2 && &runtime.locale() != local && &runtime.locale() != global,
                  "each enabled guest context owns separate locale data");
        }
        check(configure(0) == 1 && &runtime.locale() == local,
              "nested guest context exit restores the original mode and locale storage");
        check(configure(2) == 1 && configure(0) == 2 && &runtime.locale() == global,
              "disabling thread locale returns the previous mode and restores the shared locale");
    }
    check(&runtime.locale() == global, "native scope observes the shared locale outside guest activation");
    {
        auto active = first.activate();
        check(configure(0) == 2, "guest context retains its final disabled mode across activation scopes");
    }
    // Cleanup ownership is retained by the reviewed FLS hook even if the facade
    // is destroyed before its guest context. No Runtime pointer is captured.
    ambrosia::guest::ThreadContext retained;
    {
        CrtLocales temporary;
        auto active = retained.activate();
        check(temporary.configure(1) == 2 && temporary.current().codepage() == 0,
              "private locale cleanup state survives an earlier facade destruction");
    }
}
