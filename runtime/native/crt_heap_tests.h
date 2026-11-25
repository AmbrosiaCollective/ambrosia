void crt_heap_checks() {
    using namespace ambrosia::services;
    CrtHeap bounded(64);
    auto first = bounded.allocate(32);
    check(first.address && first.error == HeapError::none &&
              reinterpret_cast<std::uintptr_t>(first.address) % 16 == 0,
          "CRT allocation owns writable storage aligned for x64 values");
    if (!first.address) {
        throw std::runtime_error("synthetic heap allocation failed");
    }
    std::memset(first.address, 0x5a, 32);
    auto second = bounded.allocate(32, true);
    check(second.address && second.error == HeapError::none &&
              std::all_of(static_cast<std::byte *>(second.address),
                          static_cast<std::byte *>(second.address) + 32,
                          [](auto value) { return value == std::byte{0}; }),
          "zero allocation clears the entire requested extent");
    check(bounded.allocate(1).error == HeapError::exhausted, "CRT heap tracks total allocation budget");
    const auto failed_growth = bounded.resize(first.address, 33);
    check(!failed_growth.address && failed_growth.error == HeapError::exhausted &&
              static_cast<unsigned char *>(first.address)[31] == 0x5a,
          "failed realloc preserves the original live allocation and its contents");
    check(bounded.release(second.address), "owned allocation can be released");
    auto grown = bounded.resize(first.address, 48);
    check(grown.address && grown.error == HeapError::none &&
              std::all_of(static_cast<unsigned char *>(grown.address),
                          static_cast<unsigned char *>(grown.address) + 32,
                          [](auto value) { return value == 0x5a; }),
          "realloc growth preserves the original byte prefix");
    if (!grown.address) {
        throw std::runtime_error("synthetic realloc growth failed");
    }
    auto shrunk = bounded.resize(grown.address, 8);
    check(shrunk.address && static_cast<unsigned char *>(shrunk.address)[7] == 0x5a,
          "realloc shrink preserves the remaining byte prefix");
    if (!shrunk.address) {
        throw std::runtime_error("synthetic realloc shrink failed");
    }
    check(bounded.release(shrunk.address) && !bounded.release(shrunk.address),
          "CRT heap rejects a repeated free without forwarding it to libc");
    std::uint64_t foreign = 0;
    check(!bounded.release(&foreign) && bounded.resize(&foreign, 8).error == HeapError::foreign_pointer,
          "foreign pointers are rejected without dereference or libc realloc/free");
    auto empty = bounded.allocate(0);
    check(empty.address && bounded.release(empty.address),
          "zero-size malloc returns a distinct freeable allocation");
    auto from_null = bounded.resize(nullptr, 8);
    check(from_null.address && bounded.resize(from_null.address, 0).error == HeapError::none,
          "realloc null allocates and realloc zero releases");
    check(bounded.release(nullptr), "free null is a no-op");
    CrtHeap unavailable(0);
    check(unavailable.allocate(0).error == HeapError::exhausted,
          "even zero-size allocations respect a zero-byte heap budget");

    Runtime runtime;
    Scope scope(runtime);
    using Allocate = void *(AMBROSIA_GUEST_ABI *)(std::size_t);
    using Calloc = void *(AMBROSIA_GUEST_ABI *)(std::size_t, std::size_t);
    using Realloc = void *(AMBROSIA_GUEST_ABI *)(void *, std::size_t);
    using Free = void(AMBROSIA_GUEST_ABI *)(void *);
    using ErrorAddress = std::int32_t *(AMBROSIA_GUEST_ABI *)();
    using GetError = std::int32_t(AMBROSIA_GUEST_ABI *)(void *);
    using SetError = std::int32_t(AMBROSIA_GUEST_ABI *)(std::int32_t);
    const auto malloc_call = reinterpret_cast<Allocate>(
        runtime.resolve(service_symbol("malloc", "api-ms-win-crt-heap-l1-1-0.dll")));
    const auto calloc_call =
        reinterpret_cast<Calloc>(runtime.resolve(service_symbol("calloc", "ucrtbase.dll")));
    const auto realloc_call =
        reinterpret_cast<Realloc>(runtime.resolve(service_symbol("realloc", "ucrtbase.dll")));
    const auto free_call = reinterpret_cast<Free>(runtime.resolve(service_symbol("free", "ucrtbase.dll")));
    const auto errno_call = reinterpret_cast<ErrorAddress>(
        runtime.resolve(service_symbol("_errno", "api-ms-win-crt-runtime-l1-1-0.dll")));
    const auto get_errno =
        reinterpret_cast<GetError>(runtime.resolve(service_symbol("_get_errno", "ucrtbase.dll")));
    const auto set_errno =
        reinterpret_cast<SetError>(runtime.resolve(service_symbol("_set_errno", "ucrtbase.dll")));
    auto *fallback = errno_call();
    set_errno(59);
    ambrosia::guest::ThreadContext context;
    ambrosia::guest::ThreadContext other;
    {
        auto active = context.activate();
        auto *error = errno_call();
        check(error != fallback && *error == 0,
              "guest CRT errno starts independently of host fallback state");
        check(set_errno(17) == 0 && *error == 17, "CRT errno ABI returns writable per-context storage");
        set_last_error(83);
        auto *memory = static_cast<unsigned char *>(calloc_call(4, 8));
        check(memory && memory[0] == 0 && memory[31] == 0 && *error == 17 && get_last_error() == 83,
              "calloc Microsoft ABI clears storage and preserves successful error state");
        if (!memory) {
            throw std::runtime_error("synthetic calloc ABI failed");
        }
        memory[0] = 0x6b;
        auto *resized = static_cast<unsigned char *>(realloc_call(memory, 64));
        check(resized && resized[0] == 0x6b, "realloc ABI preserves guest data");
        free_call(resized ? resized : memory);
        check(!calloc_call(SIZE_MAX, 2) && *error == 12 && get_last_error() == 83,
              "calloc multiplication overflow reports guest ENOMEM without changing last error");
        check(!malloc_call(SIZE_MAX) && *error == 12,
              "oversize malloc fails without host allocation and reports guest ENOMEM");
        std::int32_t copy = -1;
        check(get_errno(&copy) == 0 && copy == 12 && get_errno(nullptr) == 22,
              "CRT errno getters copy exactly one guest integer and reject null output");
        {
            auto nested = other.activate();
            check(errno_call() != error && *errno_call() == 0,
                  "independent guest context has distinct zero-initialized errno");
            set_errno(23);
        }
        check(errno_call() == error && *error == 12,
              "nested guest context restores the original errno location and value");
    }
    check(errno_call() == fallback && *fallback == 59,
          "guest context exit restores native-thread CRT fallback state");
}
