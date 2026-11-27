void prng_checks() {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    Scope scope(runtime);
    guest::ThreadContext first;
    auto activation = first.activate();
    using Seed = void(AMBROSIA_GUEST_ABI *)(std::uint32_t);
    using Next = std::int32_t(AMBROSIA_GUEST_ABI *)();
    const auto seed_address = runtime.resolve(service_symbol("srand", "ucrtbase.dll"));
    const auto next_address = runtime.resolve(service_symbol("rand", "ucrtbase.dll"));
    bool aliases = seed_address && next_address;
    for (const auto *module : {"msvcrt.dll", "api-ms-win-crt-utility-l1-1-0.dll"}) {
        aliases = aliases && runtime.resolve(service_symbol("srand", module)) == seed_address &&
                  runtime.resolve(service_symbol("rand", module)) == next_address;
    }
    check(aliases, "CRT seed/random aliases resolve to typed Windows x64 services");
    if (!aliases) {
        return;
    }
    const auto seed = reinterpret_cast<Seed>(seed_address);
    const auto next = reinterpret_cast<Next>(next_address);
    const auto saved_error = errno;
    errno = E2BIG;
    crt_error() = 19;
    set_last_error(83);
    std::array<std::int32_t, 5> values{};
    for (auto &value : values) {
        value = next();
    }
    check(values == std::array<std::int32_t, 5>{41, 18467, 6334, 26500, 19169},
          "a fresh guest context starts with the exact Windows default seed-one sequence");
    seed(1);
    check(next() == 41 && next() == 18467, "reseeding rewinds the current thread's deterministic sequence");
    seed(0);
    check(next() == 38 && next() == 7719 && next() == 21238,
          "zero is a real seed rather than an implicit request for the default seed");
    seed(UINT32_MAX);
    check(next() == 35 && next() == 29739 && next() == 3374,
          "maximum unsigned seed wraps with Windows32-bit arithmetic and15-bit results");
    seed(1);
    const auto first_value = next();
    {
        guest::ThreadContext independent;
        auto nested = independent.activate();
        const auto default_value = next();
        seed(0);
        check(default_value == 41 && next() == 38,
              "another guest context begins independently and owns its reseeded state");
    }
    check(first_value == 41 && next() == 18467,
          "restoring the original guest context resumes its untouched random sequence");
    using Secure = std::int32_t(AMBROSIA_GUEST_ABI *)(void *);
    const auto secure = reinterpret_cast<Secure>(runtime.resolve(service_symbol("rand_s", "ucrtbase.dll")));
    seed(1);
    std::uint32_t entropy = 0;
    check(next() == 41 && secure(&entropy) == 0 && next() == 18467,
          "OS entropy calls leave deterministic CRT PRNG state unchanged");
    check(errno == E2BIG && crt_error() == 19 && get_last_error() == 83,
          "seed/random operations preserve host errno and both guest error domains");
    auto bytes = fixture();
    text(bytes, 0x440, "ucrtbase.dll");
    text(bytes, 0x4a2, "srand");
    text(bytes, 0x4c2, "rand");
    put<std::uint64_t>(bytes, 0x468, 0x20c0);
    constexpr unsigned char code[] = {0x48, 0x83, 0xec, 0x28, 0xb9, 1, 0, 0, 0,    0xff, 0x15, 0,    0,
                                      0,    0,    0xff, 0x15, 0,    0, 0, 0, 0x48, 0x83, 0xc4, 0x28, 0xc3};
    std::memcpy(bytes.data() + 0x200, code, sizeof(code));
    put<std::uint32_t>(bytes, 0x20b, 0x2080 - (0x1000 + 15));
    put<std::uint32_t>(bytes, 0x211, 0x2088 - (0x1000 + 21));
    auto image = pe::Image::load(bytes, [&](const auto &symbol) { return runtime.resolve(symbol); });
    check(image.run_bootstrap() == 41 && next() == 18467,
          "an original PE seeds and advances the same guest state through two real Windows imports");
    errno = saved_error;
}

void prng_worker_check() {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    Scope scope(runtime);
    guest::ThreadContext parent;
    auto activation = parent.activate();
    using Seed = void(AMBROSIA_GUEST_ABI *)(std::uint32_t);
    using Next = std::int32_t(AMBROSIA_GUEST_ABI *)();
    const auto seed = reinterpret_cast<Seed>(runtime.resolve(service_symbol("srand", "ucrtbase.dll")));
    const auto next = reinterpret_cast<Next>(runtime.resolve(service_symbol("rand", "ucrtbase.dll")));
    const auto saved_error = errno;
    errno = EDOM;
    crt_error() = 42;
    set_last_error(91);
    seed(UINT32_MAX);
    const auto parent_first = next();
    std::array<std::int32_t, 2> child_values{};
    bool child_errors_preserved = false;
    std::thread child([&] {
        try {
            Scope child_scope(runtime);
            guest::ThreadContext context;
            auto child_activation = context.activate();
            errno = E2BIG;
            crt_error() = 33;
            set_last_error(55);
            child_values[0] = next();
            seed(0);
            child_values[1] = next();
            child_errors_preserved = errno == E2BIG && crt_error() == 33 && get_last_error() == 55;
        } catch (...) {
            child_errors_preserved = false;
        }
    });
    child.join();
    check(parent_first == 35 && next() == 29739 && child_values == std::array<std::int32_t, 2>{41, 38} &&
              child_errors_preserved && errno == EDOM && crt_error() == 42 && get_last_error() == 91,
          "a joined native worker starts fresh and cannot replace its parent's seed or error state");
    errno = saved_error;
}

int prng_rejection_probe(unsigned mode) {
    using namespace ambrosia::services;
    Runtime runtime;
    Scope scope(runtime);
    if (mode == 1) {
        using Seed = void(AMBROSIA_GUEST_ABI *)(std::uint32_t);
        reinterpret_cast<Seed>(runtime.resolve(service_symbol("srand", "ucrtbase.dll")))(1);
    } else if (mode == 2) {
        using Next = std::int32_t(AMBROSIA_GUEST_ABI *)();
        (void)reinterpret_cast<Next>(runtime.resolve(service_symbol("rand", "ucrtbase.dll")))();
    } else {
        return 2;
    }
    return 1;
}
