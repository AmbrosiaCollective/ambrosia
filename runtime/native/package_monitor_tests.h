struct PackageMonitorFixture {
    ambrosia::services::Runtime runtime;
    std::filesystem::path root;
    void *object = nullptr;

    explicit PackageMonitorFixture(const char *name) : root(std::filesystem::path("/work") / name) {
        std::filesystem::create_directories(root / "assets");
        write(root / "fixture.exe", "original image");
        write(root / "assets/payload.bin", "abcde");
        write(root / "assets/empty.bin", "");
        const std::string identity =
            "<Identity Name='Ambrosia.Monitor' Publisher='CN=Example' Version='1.2.3.4'/>";
        write(root / "assets/MicrosoftGame.Config", "<Game>" + identity + "</Game>");
        const auto inventory = "<Package>" + identity +
                               "<Inventory><File Path='payload.bin' Size='5'/>"
                               "<File Path='empty.bin' Size='0'/></Inventory></Package>";
        runtime.files().bind(root / "assets", root / "fixture.exe");
        runtime.gdk_packages().bind(runtime.files());
        runtime.gdk_packages().bind_installation(std::as_bytes(std::span(inventory.data(), inventory.size())),
                                                 runtime.files());
        if (runtime.gdk_packages().query(ambrosia::services::package_interface_id, &object)) {
            throw std::runtime_error("cannot query original monitor fixture interface");
        }
    }

    ~PackageMonitorFixture() {
        (void)ambrosia::services::interface_release(object);
    }

    static void write(const std::filesystem::path &path, std::string_view text) {
        std::ofstream stream(path, std::ios::binary);
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!stream) {
            throw std::runtime_error("cannot write original installation monitor fixture");
        }
    }
};

void package_monitor_checks() {
    using namespace ambrosia;
    using namespace services;
    PackageMonitorFixture fixture("manual-package-monitors");
    Scope scope(fixture.runtime);
    auto &packages = fixture.runtime.gdk_packages();
    const auto *table = *static_cast<const std::uintptr_t *const *>(fixture.object);
    using Create = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, const char *, std::uint32_t, const void *,
                                                      std::uint32_t, std::uintptr_t, void *);
    using Close = void(AMBROSIA_GUEST_ABI *)(void *, std::uintptr_t);
    using Progress = void(AMBROSIA_GUEST_ABI *)(void *, std::uintptr_t, void *);
    using Update = bool(AMBROSIA_GUEST_ABI *)(void *, std::uintptr_t);
    const auto create = reinterpret_cast<Create>(table[5]);
    const auto close = reinterpret_cast<Close>(table[6]);
    const auto progress = reinterpret_cast<Progress>(table[7]);
    const auto update = reinterpret_cast<Update>(table[8]);
    std::array<std::byte, 16> output;
    output.fill(std::byte{0xcc});
    const auto saved_errno = errno;
    errno = EDOM;
    set_last_error(91);
    crt_error() = 61;
    const auto created = create(fixture.object, "Ambrosia.Monitor", 0, nullptr, 0, 0, output.data() + 3);
    std::uintptr_t handle = 0;
    std::memcpy(&handle, output.data() + 3, sizeof(handle));
    std::array<std::byte, 40> buffer;
    buffer.fill(std::byte{0xcc});
    progress(fixture.object, handle, buffer.data() + 3);
    InstallationProgress observed;
    std::memcpy(&observed, buffer.data() + 3, sizeof(observed));
    check(created == 0 && handle && output[2] == std::byte{0xcc} && output[11] == std::byte{0xcc} &&
              buffer[2] == std::byte{0xcc} && buffer[35] == std::byte{0xcc} && observed.total_bytes == 5 &&
              observed.installed_bytes == 5 && observed.launch_bytes == 5 && observed.launchable &&
              observed.completed && observed.reserved == std::array<std::byte, 6>{},
          "actual console monitor vtable owns a handle and writes packed guarded32-byte real progress");
    check(errno == EDOM && get_last_error() == 91 && crt_error() == 61,
          "monitor creation/progress preserve native, Windows and CRT error channels");
    PackageMonitorFixture::write(fixture.root / "assets/payload.bin", "ab");
    check(packages.progress(handle).completed && update(fixture.object, handle) &&
              !packages.progress(handle).completed && !packages.progress(handle).launchable &&
              packages.progress(handle).installed_bytes == 0 && !update(fixture.object, handle),
          "zero-cadence monitors retain a snapshot until a real manual update reports changed data");
    PackageMonitorFixture::write(fixture.root / "assets/payload.bin", "abcde");
    check(update(fixture.object, handle) && packages.progress(handle).completed,
          "manual refresh observes restored exact-size files instead of inventing installation progress");
    std::filesystem::remove(fixture.root / "assets/empty.bin");
    const auto missing_empty_changed = update(fixture.object, handle);
    const auto missing_empty = packages.progress(handle);
    PackageMonitorFixture::write(fixture.root / "assets/empty.bin", "");
    check(
        missing_empty_changed && missing_empty.installed_bytes == missing_empty.total_bytes &&
            !missing_empty.completed && update(fixture.object, handle) && packages.progress(handle).completed,
        "monitor completion cannot be inferred from equal byte totals when a required empty file is missing");
    close(fixture.object, handle);
    close(fixture.object, handle);
    close(fixture.object, 0);
    check(runtime_rejected([&] { (void)packages.progress(handle); }) &&
              runtime_rejected([&] { (void)packages.update_monitor(handle); }),
          "closed monitor IDs remain stale and cannot return fabricated zero or complete progress");
    std::uintptr_t rejected = 99;
    const auto missing_queue = create(fixture.object, "Ambrosia.Monitor", 0, nullptr, 5, 0, &rejected);
    const auto no_queue_output = rejected;
    const auto selectors =
        create(fixture.object, "Ambrosia.Monitor", 1, reinterpret_cast<void *>(1), 0, 0, &rejected);
    const auto invalid_queue = create(fixture.object, "Ambrosia.Monitor", 0, nullptr, 5, 0x1234, &rejected);
    check(missing_queue == static_cast<std::int32_t>(0x80070032u) && !no_queue_output &&
              selectors == static_cast<std::int32_t>(0x80070032u) &&
              invalid_queue == static_cast<std::int32_t>(0x80070006u) && !rejected,
          "missing/invalid queues and unsupported selectors fail before publishing handles or reading "
          "selectors");
    check(create(fixture.object, nullptr, 0, nullptr, 0, 0, &rejected) == gdk_invalid_argument && !rejected &&
              create(fixture.object, "Other.Package", 0, nullptr, 0, 0, &rejected) == gdk_invalid_argument &&
              !rejected &&
              create(fixture.object, "Ambrosia.Monitor", 0, nullptr, 0, 0, nullptr) == gdk_invalid_argument,
          "invalid identifiers and null output cannot publish a monitor");
    Runtime empty;
    check(empty.gdk_packages().create_monitor("Ambrosia.Monitor", 0, 0, {}, &rejected) != 0 && !rejected,
          "a separate runtime cannot infer package installation from another runtime's identity");
    GdkPackages identity_only;
    identity_only.bind(fixture.runtime.files());
    check(identity_only.create_monitor("Ambrosia.Monitor", 0, 0, {}, &rejected) ==
                  static_cast<std::int32_t>(0x80070032u) &&
              !rejected,
          "a valid process identity without an expected inventory cannot create fake installation progress");
    std::vector<std::uintptr_t> handles;
    bool bounded = true;
    for (unsigned index = 0; index < GdkPackages::monitor_limit; ++index) {
        const auto result = create(fixture.object, "Ambrosia.Monitor", 0, nullptr, 0, 0, &rejected);
        bounded = bounded && result == 0 && rejected > handle;
        handles.push_back(rejected);
    }
    const auto exhausted = create(fixture.object, "Ambrosia.Monitor", 0, nullptr, 0, 0, &rejected);
    check(bounded && exhausted == gdk_out_of_memory && !rejected,
          "monitor ownership has a bounded live-handle budget with transactional output clearing");
    for (const auto live : handles) {
        close(fixture.object, live);
    }
    const auto recovered = create(fixture.object, "Ambrosia.Monitor", 0, nullptr, 0, 0, &rejected);
    check(recovered == 0 && rejected > handles.back(),
          "closing monitors recovers capacity without reusing stale opaque IDs");
    std::filesystem::remove(fixture.root / "assets/payload.bin");
    std::filesystem::create_symlink(fixture.root / "fixture.exe", fixture.root / "assets/payload.bin");
    check(runtime_rejected([&] { (void)packages.update_monitor(rejected); }) &&
              runtime_rejected([&] { (void)packages.progress(rejected); }),
          "a failed confined refresh invalidates old progress instead of preserving stale complete status");
    close(fixture.object, rejected);
    errno = saved_errno;
}

void periodic_package_monitor_checks() {
    using namespace ambrosia;
    using namespace services;
    using namespace std::chrono_literals;
    PackageMonitorFixture fixture("periodic-package-monitors");
    Scope scope(fixture.runtime);
    auto &packages = fixture.runtime.gdk_packages();
    auto &queues = fixture.runtime.task_queues();
    std::uintptr_t queue = 0;
    (void)queues.create(1, 1, &queue);
    const auto scheduler = queues.thread_pool_scheduler(queue);
    std::uintptr_t monitor = 0;
    const auto created = packages.create_monitor("Ambrosia.Monitor", 0, 10, scheduler, &monitor);
    queues.close(queue);
    check(created == 0 && monitor && monitor != queue && !queues.inspect(queue),
          "periodic monitors retain their actual queue and share noncolliding runtime IDs");
    const auto wait_for_completion = [&](bool completed) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (packages.progress(monitor).completed == completed) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return false;
    };
    PackageMonitorFixture::write(fixture.root / "assets/payload.bin", "a");
    check(wait_for_completion(false), "queued periodic refresh sees a real file become incomplete");
    PackageMonitorFixture::write(fixture.root / "assets/payload.bin", "abcde");
    check(wait_for_completion(true),
          "queued periodic refresh sees real restored readiness after queue close");
    packages.close_monitor(monitor);
    bool recovered = true;
    for (unsigned index = 0; index <= host::WorkerPool::pending_limit; ++index) {
        std::uintptr_t transient = 0;
        const auto result =
            packages.create_monitor("Ambrosia.Monitor", 0, 0xffffffffu, scheduler, &transient);
        recovered = recovered && result == 0 && transient;
        packages.close_monitor(transient);
    }
    check(recovered, "closing future monitors cancels pending ownership across the timer-capacity boundary");
    (void)packages.create_monitor("Ambrosia.Monitor", 0, 0xffffffffu, scheduler, &monitor);
    check(monitor && queues.workers() == 2,
          "runtime teardown retains bounded worker count and need not wait for a distant monitor deadline");
}
