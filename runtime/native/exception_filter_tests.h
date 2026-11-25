unsigned exception_filter_calls = 0;

std::int32_t AMBROSIA_GUEST_ABI original_exception_filter(void *) {
    ++exception_filter_calls;
    return 0;
}

void exception_filter_checks() {
    using namespace ambrosia;
    using namespace services;
    ExceptionFilter filter;
    check(filter.current() == 0 && filter.replace(0) == 0,
          "top-level filter starts with default handling and null replacement returns null");
    check(filter.replace(0x1234) == 0 && filter.current() == 0x1234 && filter.replace(0x5678) == 0x1234 &&
              filter.current() == 0x5678,
          "filter replacement returns the immediately previous value and retains the new value");
    check(filter.replace(0x5678) == 0x5678 && filter.replace(0) == 0x5678 && filter.current() == 0,
          "repeated registration and null reset preserve exact previous-pointer semantics");
    check(filter.replace(UINTPTR_MAX) == 0 && filter.current() == UINTPTR_MAX &&
              filter.replace(0) == UINTPTR_MAX,
          "registration retains opaque pointer bits without validating or dereferencing them");

    Runtime runtime;
    Runtime other;
    using SetFilter = void *(AMBROSIA_GUEST_ABI *)(void *);
    const auto address = runtime.resolve(
        service_symbol("SetUnhandledExceptionFilter", "api-ms-win-core-errorhandling-l1-1-0.dll"));
    const auto set_filter = reinterpret_cast<SetFilter>(address);
    check(address &&
              address == runtime.resolve(service_symbol("SetUnhandledExceptionFilter", "kernel32.dll")) &&
              address == runtime.resolve(service_symbol("SetUnhandledExceptionFilter", "kernelbase.dll")),
          "exception-filter aliases share one callable native facade");
    const auto callback = reinterpret_cast<void *>(&original_exception_filter);
    std::uint64_t opaque = 19;
    {
        Scope scope(runtime);
        set_last_error(83);
        crt_error() = 47;
        check(set_filter(callback) == nullptr &&
                  runtime.exception_filter().current() == reinterpret_cast<std::uintptr_t>(callback),
              "Microsoft ABI registration returns the default handler and owns the callback value");
        {
            Scope nested(other);
            check(set_filter(&opaque) == nullptr &&
                      other.exception_filter().current() == reinterpret_cast<std::uintptr_t>(&opaque),
                  "another scoped guest process has an independent top-level filter");
        }
        check(set_filter(&opaque) == callback && set_filter(nullptr) == &opaque &&
                  set_filter(callback) == nullptr && get_last_error() == 83 && crt_error() == 47,
              "nested runtime scopes restore the owner and registration preserves Win32/CRT errors");
    }
    {
        pe::ModuleGraph graph(resolve);
        graph.add("app.exe", fixture());
        graph.load("app.exe");
        unsigned contexts = 0;
        for (unsigned index = 0; index < 2; ++index) {
            auto execution = graph.start();
            execution.with_guest_context([&] {
                Scope scope(runtime);
                if (index == 0) {
                    check(set_filter(&opaque) == callback,
                          "a later guest thread context observes the process-wide installed filter");
                } else {
                    check(set_filter(nullptr) == &opaque && runtime.exception_filter().current() == 0,
                          "a distinct guest context resets the same process-wide filter");
                }
                ++contexts;
            });
        }
        check(contexts == 2, "filter registration remains available across sequential guest contexts");
    }
    auto caller = fixture();
    text(caller, 0x440, "kernel32.dll");
    text(caller, 0x4a2, "SetUnhandledExceptionFilter");
    auto image = pe::Image::load(caller, [&](const auto &symbol) { return runtime.resolve(symbol); });
    {
        Scope scope(runtime);
        runtime.exception_filter().replace(reinterpret_cast<std::uintptr_t>(callback));
        check(image.run_bootstrap() == reinterpret_cast<std::uintptr_t>(callback) &&
                  runtime.exception_filter().replace(0) == 1,
              "an original PE import preserves the full previous callback and installs its opaque argument");
    }
    check(other.exception_filter().replace(0) == reinterpret_cast<std::uintptr_t>(&opaque),
          "resetting one guest process does not reset another process's filter");
    check(exception_filter_calls == 0 && opaque == 19,
          "installing and clearing callbacks never invokes them or mutates pointed-to storage");
    check(!runtime.resolve(service_symbol("UnhandledExceptionFilter", "kernel32.dll")) &&
              !runtime.resolve(service_symbol("RaiseException", "kernel32.dll")),
          "filter registration does not claim exception dispatch or raised-exception support");
}
