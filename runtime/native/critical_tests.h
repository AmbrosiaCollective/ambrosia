void critical_section_checks() {
    using namespace ambrosia::services;
    alignas(8) std::array<std::byte, 48> object{};
    auto *address = object.data();
    object.back() = std::byte{0x55};
    CriticalSections sections;
    check(sections.initialize(nullptr, 0, 0) == error_invalid_parameter,
          "critical-section initialization rejects null address");
    check(sections.initialize(address + 1, 0, 0) == error_invalid_parameter,
          "critical-section initialization rejects unaligned address");
    check(sections.initialize(address, 0, 1) == error_not_supported,
          "unsupported critical-section flags fail explicitly");
    check(sections.initialize(address, 123, 0x01000000) == 0 && sections.size() == 1,
          "critical-section initialization registers one owned native mutex");
    std::int32_t lock_count = 0, recursion = 0;
    std::uint64_t owner = 99, spin = 0;
    auto read_fields = [&] {
        std::memcpy(&lock_count, address + 8, 4);
        std::memcpy(&recursion, address + 12, 4);
        std::memcpy(&owner, address + 16, 8);
        std::memcpy(&spin, address + 32, 8);
    };
    read_fields();
    check(lock_count == -1 && recursion == 0 && owner == 0 && spin == 123 && object.back() == std::byte{0x55},
          "initial guest ABI fields fit exactly the 40-byte opaque object");
    check(sections.initialize(address, 0, 0) == error_invalid_parameter,
          "duplicate critical-section initialization is rejected");
    check(runtime_rejected([&] { sections.leave(address); }), "unowned critical-section leave fails");
    check(sections.enter(address), "native critical section acquires ownership");
    check(sections.enter(address, true), "try-enter is recursive on the calling owner thread");
    read_fields();
    check(recursion == 2 && lock_count == 1 && owner == ambrosia::host::current_thread_id(),
          "recursive entries retain actual calling-thread ownership");
    check(runtime_rejected([&] { sections.remove(address); }), "owned critical section cannot be deleted");
    sections.leave(address);
    read_fields();
    check(recursion == 1 && owner != 0, "one leave preserves ownership until recursion reaches zero");
    check(sections.set_spin(address, 77) == 123, "spin-count update reports the prior hint");
    sections.leave(address);
    read_fields();
    check(recursion == 0 && owner == 0 && lock_count == -1 && spin == 77,
          "final leave releases the native mutex and guest ownership");
    sections.remove(address);
    check(sections.size() == 0 && runtime_rejected([&] { (void)sections.enter(address); }),
          "deleted critical section has no remaining runtime registration");
    check(sections.initialize(address, 0, 0) == 0, "deleted critical section can be initialized again");
    sections.remove(address);

    Runtime runtime;
    using Initialize = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, std::uint32_t, std::uint32_t);
    using Operation = void(AMBROSIA_GUEST_ABI *)(void *);
    using TryEnter = std::int32_t(AMBROSIA_GUEST_ABI *)(void *);
    const auto initialize =
        reinterpret_cast<Initialize>(runtime.resolve(service_symbol("InitializeCriticalSectionEx")));
    const auto enter = reinterpret_cast<Operation>(runtime.resolve(service_symbol("EnterCriticalSection")));
    const auto leave = reinterpret_cast<Operation>(runtime.resolve(service_symbol("LeaveCriticalSection")));
    const auto remove = reinterpret_cast<Operation>(runtime.resolve(service_symbol("DeleteCriticalSection")));
    const auto try_enter =
        reinterpret_cast<TryEnter>(runtime.resolve(service_symbol("TryEnterCriticalSection")));
    check(!initialize(address, 0, 0) && get_last_error() == error_invalid_function,
          "guest critical sections require an active service runtime");
    {
        Scope scope(runtime);
        set_last_error(44);
        check(initialize(address, 0, 0) == 1 && get_last_error() == 44,
              "guest initialization succeeds without clearing last error");
        enter(address);
        check(try_enter(address) == 1, "Microsoft ABI recursive try-enter reaches the same native object");
        leave(address);
        leave(address);
        remove(address);
        check(runtime.critical_sections().size() == 0, "guest delete releases the owned registration");
    }
    check(
        runtime.resolve(service_symbol("InitializeCriticalSectionEx", "api-ms-win-core-synch-l1-1-0.dll")) ==
            reinterpret_cast<std::uintptr_t>(initialize),
        "observed synchronization API-set alias binds the implemented initializer");
}
