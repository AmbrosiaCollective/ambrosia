void persistent_storage_checks() {
    using namespace ambrosia;
    using namespace services;
    const auto root = std::filesystem::path("/work/persistent-storage");
    const auto assets = root / "assets";
    const auto image = root / "bin" / "original.exe";
    const auto state = root / std::filesystem::path(u8"state-é");
    std::filesystem::create_directories(assets);
    std::filesystem::create_directories(image.parent_path());
    std::filesystem::create_directories(state);
    {
        std::ofstream stream(state / "original.txt", std::ios::binary);
        stream << "fixture";
    }
    Runtime runtime;
    Scope scope(runtime);
    guest::ThreadContext thread;
    auto activation = thread.activate();
    check(!runtime.persistent_storage().path(),
          "unconfigured persistent storage reports no invented directory");
    check(runtime_rejected([&] { host::LocalStorageDirectory directory("/src"); }) &&
              runtime_rejected([&] { host::LocalStorageDirectory directory(state / "missing"); }) &&
              runtime_rejected([&] { host::LocalStorageDirectory directory(state / "original.txt"); }),
          "the host adapter refuses read-only, missing and nondirectory roots");
    std::filesystem::create_directory_symlink(state, root / "alias");
    check(runtime_rejected([&] { host::LocalStorageDirectory directory(root / "alias"); }),
          "persistent roots reject symlink aliases before any writable probe");
    const auto saved_errno = errno;
    errno = EDOM;
    crt_error() = 61;
    set_last_error(91);
    runtime.files().bind(assets, image);
    check(runtime_rejected([&] { runtime.files().bind_additional_native_root(assets); }) &&
              runtime_rejected([&] { runtime.files().bind_additional_native_root(assets / ".."); }) &&
              runtime_rejected([&] { runtime.files().bind_additional_native_root(assets / "child"); }),
          "additional roots cannot alias or contain asset roots and must exist");
    runtime.files().bind_additional_native_root(state);
    runtime.persistent_storage().bind(state);
    const auto expected_path = state.generic_string();
    check(runtime.persistent_storage().path() == expected_path &&
              runtime_rejected([&] { runtime.persistent_storage().bind(state); }) &&
              std::distance(std::filesystem::directory_iterator(state),
                            std::filesystem::directory_iterator{}) == 1,
          "real writable storage binds once and removes its exclusive empty write probe");
    auto resolved = runtime.files().resolve((state / "original.txt").generic_string());
    auto file = runtime.files().open((state / "original.txt").generic_string());
    std::array<std::byte, 7> contents{};
    check(!resolved.error && file.file && file.file->read(0, contents).bytes == 7 &&
              std::memcmp(contents.data(), "fixture", contents.size()) == 0 &&
              runtime.files().resolve("T:/original.txt").directory != resolved.directory,
          "native absolute state paths resolve real data while DOS aliases still belong to assets");
    check(runtime.gdk_bootstrap().initialize(1, 0) == 0,
          "the storage interface uses real offline GDK initialization");
    using Query = std::int32_t(AMBROSIA_GUEST_ABI *)(const Guid *, const Guid *, void **);
    const auto query =
        reinterpret_cast<Query>(runtime.resolve(service_symbol("QueryApiImpl", "xgameruntime.dll")));
    void *object = nullptr;
    check(query(&persistent_storage_class_id, &persistent_storage_interface_3, &object) == 0 && object,
          "the observed persistent-storage class and third interface resolve to a counted native object");
    using Size = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, std::size_t *);
    using Path = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, std::size_t, char *, std::size_t *);
    const auto table = *static_cast<const std::uintptr_t **>(object);
    const auto get_size = reinterpret_cast<Size>(table[3]);
    const auto get_path = reinterpret_cast<Path>(table[4]);
    errno = EDOM;
    crt_error() = 61;
    set_last_error(91);
    std::size_t size = 999;
    check(get_size(object, &size) == 0 && size == expected_path.size() + 1 &&
              get_size(object, nullptr) == pointer_error,
          "path sizing returns the dynamic UTF-8 byte count including its terminator");
    std::array<char, 256> output{};
    std::size_t used = 999;
    check(get_path(object, output.size(), output.data(), &used) == 0 &&
              std::string(output.data()) == expected_path && used == size &&
              get_path(object, output.size(), output.data(), nullptr) == 0,
          "the measured optional-used-pointer path call returns an actual writable directory");
    output.fill('X');
    used = 999;
    check(get_path(object, size - 1, output.data(), &used) == static_cast<std::int32_t>(0x8007007au) &&
              used == 0 && std::all_of(output.begin(), output.end(), [](char unit) { return unit == 'X'; }) &&
              get_path(object, size, nullptr, &used) == pointer_error && used == 0,
          "insufficient path capacity preserves destination guards and null output returns E_POINTER");
    const auto unknown = Guid{1, 2, 3, {4, 5, 6, 7, 8, 9, 10, 11}};
    void *other = nullptr;
    check(query(&persistent_storage_class_id, &persistent_storage_interface_2, &other) == 0 &&
              other == object && query(&persistent_storage_class_id, &unknown, &other) == no_interface &&
              !other,
          "supported storage revisions share identity while unrelated IIDs clear output and fail");
    check(errno == EDOM && crt_error() == 61 && get_last_error() == 91,
          "storage host operations and successful interface paths preserve native and guest errors");
    {
        Runtime independent;
        Scope independent_scope(independent);
        void *unbound = nullptr;
        independent.persistent_storage().query(persistent_storage_interface_3, &unbound);
        size = 999;
        used = 999;
        check(get_size(unbound, &size) == static_cast<std::int32_t>(0x80070003u) && size == 0 &&
                  get_path(unbound, output.size(), output.data(), &used) ==
                      static_cast<std::int32_t>(0x80070003u) &&
                  used == 0,
              "a separate unbound runtime returns an honest missing-path result with cleared counts");
    }
    std::filesystem::permissions(state,
                                 std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
    check(runtime_rejected([&] { (void)runtime.persistent_storage().path(); }),
          "removing writable permission invalidates the path instead of claiming writable storage");
    std::filesystem::permissions(state, std::filesystem::perms::owner_all);
    std::filesystem::rename(state, root / "retained-state");
    std::filesystem::create_directory(state);
    check(runtime_rejected([&] { (void)runtime.persistent_storage().path(); }),
          "replacing the configured path cannot retarget the pinned storage identity");
    std::ostringstream messages;
    std::ostringstream errors;
    check(
        run_native_command(std::vector<std::filesystem::path>{"--inspect", image, "--storage-root", state},
                           messages, errors) == 2 &&
            run_native_command(std::vector<std::filesystem::path>{"--run-bootstrap", image, "--storage-root",
                                                                  state, "--storage-root", state},
                               messages, errors) == 2,
        "storage roots require execution mode and cannot be specified twice");
    errno = saved_errno;
}
