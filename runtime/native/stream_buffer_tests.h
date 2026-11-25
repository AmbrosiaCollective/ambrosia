void stream_buffer_checks() {
    using namespace ambrosia::services;
    Runtime runtime;
    Scope scope(runtime);
    using GetBuffer = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, void *, void *, void *);
    const auto get_buffer = reinterpret_cast<GetBuffer>(
        runtime.resolve(service_symbol("_get_stream_buffer_pointers", "api-ms-win-crt-stdio-l1-1-0.dll")));
    auto *file = runtime.streams().file(1);
    char **base = nullptr;
    char **cursor = nullptr;
    std::int32_t *count = nullptr;
    set_last_error(62);
    crt_error() = 29;
    check(get_buffer(file, &base, &cursor, &count) == 0 && base && cursor && count,
          "CRT stream-buffer Microsoft ABI returns three owned field addresses");
    check(base != cursor && !*base && !*cursor && *count == 0,
          "new standard-stream buffer has distinct zero-initialized fields");
    std::array<char, 8> storage{};
    *base = storage.data();
    *cursor = storage.data() + 2;
    *count = 6;
    auto *owned = runtime.streams().buffer(file);
    check(owned && owned->base == storage.data() && owned->cursor == storage.data() + 2 &&
              owned->remaining == 6,
          "writes through exported buffer pointers update the actual owned stream state");
    char **again = nullptr;
    check(get_buffer(file, &again, nullptr, nullptr) == 0 && again == base &&
              get_buffer(file, nullptr, nullptr, nullptr) == 0,
          "buffer field addresses are stable and individual output pointers are optional");
    check(get_last_error() == 62 && crt_error() == 29,
          "successful stream-buffer query preserves Win32 and CRT errors");
    auto *input = runtime.streams().buffer(runtime.streams().file(0));
    check(input && !input->base && !input->cursor && input->remaining == 0,
          "separate standard streams retain independent buffer fields");
    Runtime foreign;
    again = base;
    check(get_buffer(foreign.streams().file(1), &again, nullptr, nullptr) == 22 && again == base &&
              crt_error() == 22,
          "foreign-runtime stream fails without following it or overwriting outputs");
    check(get_buffer(nullptr, nullptr, nullptr, nullptr) == 22 && get_last_error() == 62,
          "null stream reports CRT EINVAL without changing Win32 last error");
    *base = nullptr;
    *cursor = nullptr;
    *count = 0;
}
