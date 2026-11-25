ambrosia::services::Runtime *new_handler_owner = nullptr;
void *new_handler_reserve = nullptr;
unsigned new_handler_calls = 0;
std::size_t new_handler_bytes = 0;
unsigned new_handler_action = 0;

std::int32_t AMBROSIA_GUEST_ABI original_new_handler(std::size_t bytes) {
    ++new_handler_calls;
    new_handler_bytes = bytes;
    if (new_handler_action == 1 && new_handler_reserve) {
        new_handler_owner->heap().release(new_handler_reserve);
        new_handler_reserve = nullptr;
        return -7; // Every nonzero result requests a retry, not just +1.
    }
    if (new_handler_action == 2) {
        new_handler_owner->new_allocations().replace_handler(nullptr);
        return 1;
    }
    return new_handler_action == 3 ? -7 : 0;
}

void new_allocation_checks() {
    using namespace ambrosia::services;
    NewAllocations policy;
    check(policy.mode() == 0 && !policy.handler() && policy.replace_mode(1) == 0 && policy.mode() == 1 &&
              policy.replace_mode(0) == 1,
          "new-handler mode starts disabled and replacement returns the previous mode");
    check(policy.replace_mode(2) == -1 && policy.replace_mode(-1) == -1 && policy.mode() == 0,
          "invalid new-handler modes are rejected without changing policy state");

    Runtime runtime(MemoryLimits{4096, 4096, 4096}, 64);
    Runtime other(MemoryLimits{4096, 4096, 4096}, 64);
    Scope scope(runtime);
    new_handler_owner = &runtime;
    new_handler_calls = 0;
    new_handler_bytes = 0;
    new_handler_action = 0;
    using SetMode = std::int32_t(AMBROSIA_GUEST_ABI *)(std::int32_t);
    using QueryMode = std::int32_t(AMBROSIA_GUEST_ABI *)();
    using SetHandler = NewAllocationHandler(AMBROSIA_GUEST_ABI *)(NewAllocationHandler);
    using QueryHandler = NewAllocationHandler(AMBROSIA_GUEST_ABI *)();
    using CallHandler = std::int32_t(AMBROSIA_GUEST_ABI *)(std::size_t);
    using Allocate = void *(AMBROSIA_GUEST_ABI *)(std::size_t);
    using Calloc = void *(AMBROSIA_GUEST_ABI *)(std::size_t, std::size_t);
    using Realloc = void *(AMBROSIA_GUEST_ABI *)(void *, std::size_t);
    const auto mode_address =
        runtime.resolve(service_symbol("_set_new_mode", "api-ms-win-crt-heap-l1-1-0.dll"));
    const auto set_mode = reinterpret_cast<SetMode>(mode_address);
    const auto query_mode =
        reinterpret_cast<QueryMode>(runtime.resolve(service_symbol("_query_new_mode", "ucrtbase.dll")));
    const auto set_handler =
        reinterpret_cast<SetHandler>(runtime.resolve(service_symbol("_set_new_handler", "ucrtbase.dll")));
    const auto query_handler =
        reinterpret_cast<QueryHandler>(runtime.resolve(service_symbol("_query_new_handler", "ucrtbase.dll")));
    const auto call_handler =
        reinterpret_cast<CallHandler>(runtime.resolve(service_symbol("_callnewh", "ucrtbase.dll")));
    const auto allocate =
        reinterpret_cast<Allocate>(runtime.resolve(service_symbol("malloc", "ucrtbase.dll")));
    const auto calloc = reinterpret_cast<Calloc>(runtime.resolve(service_symbol("calloc", "ucrtbase.dll")));
    const auto realloc =
        reinterpret_cast<Realloc>(runtime.resolve(service_symbol("realloc", "ucrtbase.dll")));
    check(mode_address && mode_address == runtime.resolve(service_symbol("_set_new_mode", "ucrtbase.dll")) &&
              mode_address == runtime.resolve(service_symbol("_set_new_mode", "msvcrt.dll")),
          "CRT allocation policy aliases share one callable guest facade");
    set_last_error(83);
    crt_error() = 17;
    check(!query_handler() && !query_mode() && call_handler(4) == 0 && !set_handler(&original_new_handler) &&
              query_handler() == &original_new_handler &&
              set_handler(&original_new_handler) == &original_new_handler,
          "handler registration/query return exact previous values and default callnewh declines");
    check(set_mode(1) == 0 && query_mode() == 1 && set_mode(0) == 1 && get_last_error() == 83 &&
              crt_error() == 17,
          "valid mode switching preserves LastError and CRT errno");
    new_handler_action = 3;
    check(call_handler(0x100000001ull) == 1 && new_handler_bytes == 0x100000001ull && new_handler_calls == 1,
          "callnewh passes full x64 size and normalizes negative nonzero handler results");
    new_handler_action = 0;
    new_handler_calls = 0;
    check(!allocate(65) && new_handler_calls == 0 && crt_error() == 12,
          "disabled mode refuses exhausted allocation without invoking the registered handler");
    set_mode(1);
    check(!allocate(65) && new_handler_calls == 1 && new_handler_bytes == 65 && crt_error() == 12 &&
              get_last_error() == 83,
          "enabled mode invokes a declining handler once and reports guest ENOMEM");
    const auto calls = new_handler_calls;
    check(!allocate(SIZE_MAX) && !calloc(SIZE_MAX, 2) && !realloc(nullptr, SIZE_MAX) &&
              new_handler_calls == calls,
          "oversized and overflowing requests fail without calling a new handler");

    new_handler_reserve = runtime.heap().allocate(64).address;
    if (!new_handler_reserve) {
        throw std::runtime_error("tiny new-handler reserve unavailable");
    }
    new_handler_action = 1;
    new_handler_calls = 0;
    crt_error() = 17;
    auto *memory = allocate(16);
    check(memory && !new_handler_reserve && new_handler_calls == 1 && new_handler_bytes == 16 &&
              crt_error() == 17 && get_last_error() == 83,
          "malloc retries successfully after a handler releases storage outside the heap lock");
    runtime.heap().release(memory);
    new_handler_reserve = runtime.heap().allocate(64).address;
    memory = calloc(8, 8);
    check(memory && new_handler_bytes == 64 &&
              std::all_of(static_cast<std::byte *>(memory), static_cast<std::byte *>(memory) + 64,
                          [](auto byte) { return byte == std::byte{0}; }),
          "calloc retries after recovery and still zeroes the full requested extent");
    runtime.heap().release(memory);
    memory = allocate(24);
    if (!memory) {
        throw std::runtime_error("tiny realloc fixture unavailable");
    }
    std::memset(memory, 0x6b, 24);
    new_handler_reserve = runtime.heap().allocate(40).address;
    auto *resized = realloc(memory, 32);
    check(resized && new_handler_bytes == 32 && !new_handler_reserve &&
              static_cast<unsigned char *>(resized)[23] == 0x6b,
          "realloc retries after a handler releases other storage and preserves the original prefix");
    runtime.heap().release(resized ? resized : memory);
    new_handler_reserve = runtime.heap().allocate(64).address;
    new_handler_calls = 0;
    memory = allocate(0);
    check(memory && new_handler_bytes == 1 && new_handler_calls == 1,
          "zero-byte malloc recovery receives the normalized one-byte request");
    runtime.heap().release(memory);
    new_handler_action = 2;
    new_handler_calls = 0;
    check(!allocate(65) && new_handler_calls == 1 && !query_handler() && crt_error() == 12,
          "a handler can clear itself without deadlock and the next failed retry observes the change");
    {
        Scope nested(other);
        check(query_mode() == 0 && !query_handler() && set_mode(1) == 0,
              "another guest process starts with its own default mode and handler state");
    }
    check(query_mode() == 1 && !query_handler(),
          "nested runtime scopes restore the original allocation policy");
    new_handler_owner = nullptr;
}
