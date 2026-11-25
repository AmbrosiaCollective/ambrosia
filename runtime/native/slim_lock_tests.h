void slim_lock_checks() {
    using namespace ambrosia;
    using namespace services;
    alignas(8) std::array<std::uint64_t, 3> storage{0x1111, 0, 0x3333};
    auto *address = &storage[1];
    SlimLocks locks;
    check(locks.acquire(address, false, true) && !locks.acquire(address, false, true) &&
              !locks.acquire(address, true, true),
          "zero-initialized SRW locks register lazily; an exclusive hold excludes both acquisition modes");
    check(runtime_rejected([&] { locks.initialize(address); }) &&
              runtime_rejected([&] { locks.release(address, true); }),
          "held SRW locks reject reinitialization and wrong-mode release without releasing ownership");
    locks.release(address, false);
    check(locks.acquire(address, true, true) && locks.acquire(address, true, true) &&
              !locks.acquire(address, false, true),
          "multiple shared SRW acquisitions coexist and exclude writers");
    locks.release(address, true);
    check(!locks.acquire(address, false, true),
          "a partial shared release retains exclusion until every reader releases");
    locks.release(address, true);
    check(locks.acquire(address, false, false),
          "balanced shared releases make an SRW lock available to a writer");
    locks.release(address, false);
    locks.initialize(address);
    check(storage[0] == 0x1111 && storage[1] == 0 && storage[2] == 0x3333 &&
              runtime_rejected([&] { locks.release(address, false); }),
          "SRW initialization touches one pointer-sized guest cell; an unlocked release fails explicitly");
    check(runtime_rejected([&] { locks.initialize(nullptr); }) && runtime_rejected([&] {
              locks.acquire(reinterpret_cast<std::byte *>(address) + 1, true, true);
          }),
          "SRW directory rejects null and misaligned guest addresses");
    alignas(8) std::uint64_t foreign = 7;
    check(runtime_rejected([&] { locks.acquire(&foreign, false, true); }),
          "unowned nonzero guest SRW state is not reinterpreted as a native lock pointer");
    locks.initialize(&foreign);
    check(foreign == 0 && locks.acquire(&foreign, true, false),
          "explicit SRW initialization accepts uninitialized guest storage");
    locks.release(&foreign, true);
    bool reader_limit = false;
    for (unsigned index = 0; index < 4096; ++index) {
        (void)locks.acquire(address, true, true);
    }
    try {
        (void)locks.acquire(address, true, true);
    } catch (const std::bad_alloc &) {
        reader_limit = true;
    }
    for (unsigned index = 0; index < 4096; ++index) {
        locks.release(address, true);
    }
    check(reader_limit && locks.acquire(address, false, true),
          "SRW reader accounting is bounded and recovers after balanced release");
    locks.release(address, false);

    SlimLocks bounded;
    std::vector<std::uint64_t> directory(4097);
    for (unsigned index = 0; index < 4096; ++index) {
        bounded.initialize(&directory[index]);
    }
    bool directory_limit = false;
    try {
        bounded.initialize(&directory.back());
    } catch (const std::bad_alloc &) {
        directory_limit = true;
    }
    check(directory_limit, "native SRW side tables have a bounded process-owned address directory");

    Runtime runtime;
    Runtime independent;
    Scope scope(runtime);
    using Operation = void(AMBROSIA_GUEST_ABI *)(void *);
    using Try = std::uint8_t(AMBROSIA_GUEST_ABI *)(void *);
    const auto operation = [&](const char *name) {
        return reinterpret_cast<Operation>(runtime.resolve(service_symbol(name, "kernel32.dll")));
    };
    const auto initialize = operation("InitializeSRWLock");
    const auto exclusive = operation("AcquireSRWLockExclusive");
    const auto shared = operation("AcquireSRWLockShared");
    const auto release_exclusive = operation("ReleaseSRWLockExclusive");
    const auto release_shared = operation("ReleaseSRWLockShared");
    const auto try_exclusive =
        reinterpret_cast<Try>(runtime.resolve(service_symbol("TryAcquireSRWLockExclusive", "kernel32.dll")));
    const auto try_shared =
        reinterpret_cast<Try>(runtime.resolve(service_symbol("TryAcquireSRWLockShared", "kernel32.dll")));
    bool aliases = true;
    for (const auto *name :
         {"InitializeSRWLock", "AcquireSRWLockExclusive", "AcquireSRWLockShared", "ReleaseSRWLockExclusive",
          "ReleaseSRWLockShared", "TryAcquireSRWLockExclusive", "TryAcquireSRWLockShared"}) {
        const auto entry = runtime.resolve(service_symbol(name, "kernel32.dll"));
        aliases &= entry && entry == runtime.resolve(service_symbol(name, "kernelbase.dll")) &&
                   entry == runtime.resolve(service_symbol(name, "api-ms-win-core-synch-l1-1-0.dll"));
    }
    check(aliases, "all seven SRW APIs share typed callable kernel/API-set facades");
    const auto saved_error = errno;
    errno = E2BIG;
    crt_error() = 19;
    set_last_error(87);
    initialize(address);
    exclusive(address);
    check(!try_exclusive(address) && !try_shared(address),
          "Windows BOOLEAN try APIs report genuine contention while an exclusive SRW acquisition is held");
    check(independent.slim_locks().acquire(address, false, true),
          "independent runtimes do not share native SRW side-table state for the same guest address");
    independent.slim_locks().release(address, false);
    release_exclusive(address);
    shared(address);
    const auto admitted = try_shared(address);
    check(admitted == 1 && !try_exclusive(address),
          "shared ABI acquisitions coexist and keep try-writers excluded");
    if (admitted) {
        release_shared(address);
    }
    release_shared(address);
    check(errno == E2BIG && crt_error() == 19 && get_last_error() == 87,
          "successful and contended SRW operations preserve native errno and both guest error domains");

    auto caller = fixture();
    text(caller, 0x440, "kernel32.dll");
    text(caller, 0x4a2, "TryAcquireSRWLockExclusive");
    constexpr unsigned char code[] = {0x48, 0x83, 0xec, 0x28, 0x48, 0x8d, 0x0d, 0, 0,
                                      0,    0,    0xff, 0x15, 0,    0,    0,    0, 0x0f,
                                      0xb6, 0xc0, 0x48, 0x83, 0xc4, 0x28, 0xc3};
    std::memcpy(caller.data() + 0x200, code, sizeof(code));
    put<std::uint32_t>(caller, 0x207, 0x2110 - (0x1000 + 11));
    put<std::uint32_t>(caller, 0x20d, 0x2080 - (0x1000 + 17));
    auto image = pe::Image::load(caller, [&](const auto &symbol) { return runtime.resolve(symbol); });
    const auto first = image.run_bootstrap();
    const auto contended = image.run_bootstrap();
    check(first == 1 && contended == 0,
          "an original PE observes a real SRW acquisition then contention through the BOOLEAN ABI");
    runtime.slim_locks().release(const_cast<std::byte *>(image.base()) + 0x2110, false);
    errno = saved_error;
}
