void storage_factory_checks() {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    Runtime independent;
    Scope scope(runtime);
    using Factory = std::int32_t(AMBROSIA_GUEST_ABI *)(const void *, void *);
    const auto get =
        reinterpret_cast<Factory>(runtime.resolve(service_symbol("DStorageGetFactory", "xcrdapi.dll")));
    void *object = nullptr;
    const auto status = get(&storage_factory_x2_id, &object);
    check(status == 0 && object && runtime.storage_factory().references() == 2 &&
              independent.storage_factory().references() == 1,
          "the public console storage factory identity is owned and counted independently by each runtime");
    const auto *table = *static_cast<const std::uintptr_t *const *>(object);
    using Query = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, const void *, void *);
    using Release = std::uint32_t(AMBROSIA_GUEST_ABI *)(void *);
    using Stage = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, std::uint32_t);
    using Debug = void(AMBROSIA_GUEST_ABI *)(void *, std::uint32_t);
    using Affinity = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, std::uint64_t);
    const auto query = reinterpret_cast<Query>(table[0]);
    const auto release = reinterpret_cast<Release>(table[2]);
    const auto stage = reinterpret_cast<Stage>(table[8]);
    const auto debug = reinterpret_cast<Debug>(table[7]);
    const auto affinity = reinterpret_cast<Affinity>(table[6]);
    void *unknown = nullptr;
    const auto unknown_status = query(object, &unknown_interface_id, &unknown);
    const auto released_unknown = release(unknown);
    const Guid absent{};
    unknown = reinterpret_cast<void *>(1);
    const auto absent_status = get(&absent, &unknown);
    check(unknown_status == 0 && released_unknown == 2 && absent_status == no_interface && !unknown,
          "the console factory shares counted IUnknown identity and rejects unknown layout revisions without "
          "a guessed PC vtable");
    unknown = reinterpret_cast<void *>(1);
    const auto missing_id = get(nullptr, &unknown);
    const auto missing_output = get(&storage_factory_x2_id, nullptr);
    check(missing_id == pointer_error && !unknown && missing_output == pointer_error,
          "missing factory identifier/output pointers report E_POINTER and cannot expose a partially "
          "initialized interface");
    const auto saved_error = errno;
    errno = EDOM;
    crt_error() = 61;
    set_last_error(91);
    const auto allocated = stage(object, 65536);
    debug(object, 5);
    auto configuration = runtime.storage_factory().configuration();
    const auto other = independent.storage_factory().configuration();
    check(allocated == 0 && configuration.staging_bytes == 65536 && configuration.debug_flags == 5 &&
              other.staging_bytes == 0 && other.debug_flags == 0 && errno == EDOM && crt_error() == 61 &&
              get_last_error() == 91,
          "console slots8 and7 allocate actual owned staging memory and store debug policy without changing "
          "another runtime or caller errors");
    const auto over_budget = stage(object, StorageFactory::staging_limit + 1);
    configuration = runtime.storage_factory().configuration();
    check(over_budget == gdk_out_of_memory && configuration.staging_bytes == 65536 &&
              configuration.debug_flags == 5,
          "over-budget staging allocation fails transactionally and preserves the existing real buffer and "
          "policy");
    const auto resized = stage(object, 4096);
    const auto released = stage(object, 0);
    const auto cleared = runtime.storage_factory().configuration();
    const auto unavailable_affinity = affinity(object, 1);
    check(resized == 0 && released == 0 && cleared.staging_bytes == 0 && cleared.debug_flags == 5 &&
              unavailable_affinity == static_cast<std::int32_t>(0x80070032u) && errno == EDOM &&
              crt_error() == 61 && get_last_error() == 91,
          "staging buffers resize/release real ownership while unavailable I/O-worker affinity returns an "
          "honest capability error");
    auto bytes = fixture();
    text(bytes, 0x440, "xcrdapi.dll");
    text(bytes, 0x4a2, "DStorageGetFactory");
    const unsigned char code[] = {0x48, 0x83, 0xec, 0x28, 0x48, 0x8d, 0x0d, 0,    0,    0,
                                  0,    0x48, 0x8d, 0x15, 0,    0,    0,    0,    0xff, 0x15,
                                  0,    0,    0,    0,    0x48, 0x83, 0xc4, 0x28, 0xc3};
    std::memcpy(bytes.data() + 0x200, code, sizeof(code));
    put<std::uint32_t>(bytes, 0x207, 0x21c0 - 0x100b);
    put<std::uint32_t>(bytes, 0x20e, 0x21f0 - 0x1012);
    put<std::uint32_t>(bytes, 0x214, 0x2080 - 0x1018);
    std::memcpy(bytes.data() + 0x5c0, &storage_factory_x2_id, sizeof(storage_factory_x2_id));
    put<std::uint64_t>(bytes, 0x5f0, 0);
    auto image = pe::Image::load(bytes, [&](const auto &symbol) { return runtime.resolve(symbol); });
    const auto result = image.run_bootstrap();
    void *guest_object = nullptr;
    std::memcpy(&guest_object, image.base() + 0x21f0, sizeof(guest_object));
    const auto references = runtime.storage_factory().references();
    if (guest_object) {
        (void)release(guest_object);
    }
    const auto final_release = release(object);
    check(result == 0 && guest_object == object && references == 3 && final_release == 1 &&
              runtime.storage_factory().references() == 1,
          "original PE factory call decodes GUID/output through Microsoft ABI and balances its acquired "
          "runtime-pinned references");
    errno = saved_error;
}
