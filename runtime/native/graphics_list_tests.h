void graphics_recording_checks() {
    using namespace ambrosia;
    using namespace services;
    const std::array<std::uintptr_t, 1> table{1};
    GraphicsRecording recording(table.data());
    auto *object = static_cast<std::byte *>(recording.guest_object());
    const auto expected = reinterpret_cast<std::uintptr_t>(object + console_list_room_offset);
    std::array<std::uintptr_t, 3> bounds{};
    std::memcpy(&bounds[0], object + console_list_write_offset, sizeof(std::uintptr_t));
    std::memcpy(&bounds[1], object + console_list_dispatch_end_offset, sizeof(std::uintptr_t));
    std::memcpy(&bounds[2], object + console_list_end_offset, sizeof(std::uintptr_t));
    check(reinterpret_cast<std::uintptr_t>(object) % 64 == 0 &&
              bounds == std::array<std::uintptr_t, 3>{expected, expected, expected} &&
              recording.bounds_intact(),
          "console storage has aligned equal write/end bounds inside its owned allocation");
    auto &command_ir = recording.command_ir();
    Dispatch dispatch{};
    dispatch.pipeline = 1;
    dispatch.x = 1;
    dispatch.y = 1;
    dispatch.z = 1;
    check(command_ir.dispatch(dispatch) && !recording.reset() && command_ir.commands().size() == 1,
          "the canonical IR records data and refuses reset while open without dropping it");
    std::memset(object + 0x140, 0xa5, 8);
    check(recording.close() && command_ir.closed() && command_ir.commands().size() == 1,
          "guest cache writes do not corrupt external IR ownership or its retained recording");
    check(!recording.close() && !command_ir.dispatch(dispatch) && command_ir.commands().size() == 1,
          "closed recordings reject another close and further records");
    check(recording.reset() && !recording.closed() && command_ir.commands().empty() &&
              recording.bounds_intact(),
          "a closed recording resets the canonical IR and keeps the fallback bounds intact");
    check(command_ir.dispatch(dispatch), "a reset recording accepts new canonical IR data");
    const auto changed = expected + sizeof(std::uint32_t);
    std::memcpy(object + console_list_write_offset, &changed, sizeof(changed));
    bool rejected = false;
    try {
        (void)recording.close();
    } catch (const std::runtime_error &error) {
        rejected = std::string_view(error.what()).find("bounds changed") != std::string_view::npos;
    }
    check(rejected && !recording.closed() && command_ir.commands().size() == 1,
          "unexpected inline buffer progress fails before close and preserves known IR records");
}

void graphics_list_live_checks(const std::filesystem::path &root, unsigned probe = 0) {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    Scope scope(runtime);
    guest::ThreadContext context;
    auto activation = context.activate();
    runtime.graphics_devices().bind(root);
    void *device = nullptr;
    check(runtime.graphics_devices().create(&device11_interface, &device, std::nullopt) == 0 && device,
          "list fixture owns a real backend device");
    auto *device_table = *static_cast<const std::uintptr_t **>(device);
    using CreateAllocator = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, std::uint32_t, const void *, void **);
    using CreateList =
        std::int32_t(AMBROSIA_GUEST_ABI *)(void *, const void *, void *, const void *, const void *, void **);
    using Query = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, const void *, void **);
    using Reference = std::uint32_t(AMBROSIA_GUEST_ABI *)(void *);
    using Close = std::int32_t(AMBROSIA_GUEST_ABI *)(void *);
    using Reset = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, void *, const void *);
    const auto create_allocator = reinterpret_cast<CreateAllocator>(device_table[9]);
    const auto create_list = reinterpret_cast<CreateList>(device_table[73]);
    const auto release_device = reinterpret_cast<Reference>(device_table[2]);
    void *allocator = nullptr;
    check(create_allocator(device, 0, &command_allocator_interface, &allocator) == 0 && allocator,
          "the first CPU allocator retains the device before recording starts");
    auto *allocator_table = *static_cast<const std::uintptr_t **>(allocator);
    const auto reset_allocator = reinterpret_cast<Close>(allocator_table[8]);
    const auto release_allocator = reinterpret_cast<Reference>(allocator_table[2]);
    const std::array<std::uint32_t, 4> description{};
    void *output = reinterpret_cast<void *>(1);
    const Guid unknown{};
    const auto saved_errno = errno;
    errno = E2BIG;
    set_last_error(89);
    crt_error() = 37;
    check(create_list(device, reinterpret_cast<void *>(1), reinterpret_cast<void *>(1), nullptr,
                      reinterpret_cast<void *>(1), nullptr) == pointer_error,
          "list creation checks output before reading any caller data");
    check(create_list(device, nullptr, allocator, nullptr, &console_command_list_interface, &output) ==
                  pointer_error &&
              !output,
          "list creation clears output before rejecting a missing descriptor");
    check(create_list(device, description.data(), allocator, nullptr, nullptr, &output) == pointer_error &&
              !output,
          "list creation rejects a missing interface without an owned object");
    check(create_list(device, description.data(), allocator, nullptr, &unknown, &output) == no_interface &&
              !output,
          "unmodelled list interfaces are refused before creation");
    check(create_list(device, description.data(), reinterpret_cast<void *>(1), nullptr,
                      &console_command_list_interface, &output) == static_cast<std::int32_t>(0x80070057u) &&
              !output,
          "unowned allocator pointers are refused without dereference");
    const std::array<std::uint32_t, 4> other_type{2, 0, 0, 0};
    check(create_list(device, other_type.data(), allocator, nullptr, &console_command_list_interface,
                      &output) == static_cast<std::int32_t>(0x80070057u) &&
              !output,
          "list and allocator types must agree");
    check(create_list(device, description.data(), allocator, reinterpret_cast<void *>(1),
                      &console_command_list_interface, &output) == static_cast<std::int32_t>(0x80004001u) &&
              !output,
          "unsupported initial pipelines are not silently ignored");
    void *list = nullptr;
    check(create_list(device, description.data(), allocator, nullptr, &console_command_list_interface,
                      &list) == 0 &&
              list,
          "the console interface receives actual owned storage and canonical CPU recording state");
    auto *table = *static_cast<const std::uintptr_t **>(list);
    const auto query = reinterpret_cast<Query>(table[0]);
    const auto add = reinterpret_cast<Reference>(table[1]);
    const auto release = reinterpret_cast<Reference>(table[2]);
    const auto get_device = reinterpret_cast<Query>(table[7]);
    const auto get_type = reinterpret_cast<Reference>(table[8]);
    const auto close = reinterpret_cast<Close>(table[9]);
    const auto reset = reinterpret_cast<Reset>(table[10]);
    if (probe == 6) {
        diagnostics::ImportStops stops;
        stops.seal();
        diagnostics::ImportStopScope trace(stops);
        using Capture =
            void(AMBROSIA_GUEST_ABI *)(void *, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
        reinterpret_cast<Capture>(table[132])(list, 0x12345678u, 0xabcdef01u, 0, 0xffffffffff00ull);
        throw std::runtime_error("register diagnostic unexpectedly returned");
    }
    if (probe == 1 || probe == 5) {
        const auto unsupported = probe == 1 ? 12u : 599u;
        reinterpret_cast<Close>(table[unsupported])(list);
        throw std::runtime_error("unsupported list command unexpectedly returned");
    }
    if (probe == 2) {
        Runtime other;
        Scope foreign(other);
        (void)close(list);
        throw std::runtime_error("foreign-runtime list unexpectedly closed");
    }
    if (probe == 4) {
        std::uintptr_t write;
        auto *bytes = static_cast<std::byte *>(list);
        std::memcpy(&write, bytes + console_list_write_offset, sizeof(write));
        write += 4;
        std::memcpy(bytes + console_list_write_offset, &write, sizeof(write));
        (void)close(list);
        throw std::runtime_error("changed inline buffer bounds unexpectedly closed");
    }
    check(get_type(list) == 0 && reset(list, allocator, nullptr) == static_cast<std::int32_t>(0x80004005u),
          "a newly open direct list cannot reset before closing");
    check(reset_allocator(allocator) == static_cast<std::int32_t>(0x80004005u),
          "an allocator remains busy while its list records");
    check(create_list(device, description.data(), allocator, nullptr, &console_command_list_interface,
                      &output) == static_cast<std::int32_t>(0x80070057u) &&
              !output,
          "a second list cannot acquire an allocator already recording");
    check(query(list, nullptr, &output) == pointer_error && !output &&
              query(list, &unknown, &output) == no_interface && !output,
          "list QueryInterface clears failed outputs");
    check(query(list, &console_command_list_interface, &output) == 0 && output == list &&
              release(output) == 1 && add(list) == 2 && release(list) == 1,
          "console list identity owns stable independent references");
    check(query(list, &graphics_command_list_interface, &output) == 0 && output == list &&
              release(output) == 1,
          "public base list queries preserve the console-shaped object");
    check(get_device(list, &device11_interface, &output) == 0 && output == device &&
              release_device(output) == 3,
          "list GetDevice returns the live parent with another reference");
    check(close(list) == 0 && close(list) == static_cast<std::int32_t>(0x80004005u) &&
              reset_allocator(allocator) == 0,
          "Close releases the recording lease and duplicate Close fails");
    check(reset(list, allocator, nullptr) == 0 &&
              reset_allocator(allocator) == static_cast<std::int32_t>(0x80004005u) && close(list) == 0,
          "Reset reacquires the allocator only for the new open recording");
    void *second_allocator = nullptr;
    check(create_allocator(device, 0, &command_allocator_interface, &second_allocator) == 0 &&
              second_allocator,
          "a second allocator supplies an independent recording owner");
    void *second_list = nullptr;
    check(create_list(device, description.data(), second_allocator, nullptr, &console_command_list_interface,
                      &second_list) == 0 &&
              second_list,
          "another list can record independently with another allocator");
    check(reset(list, second_allocator, nullptr) == static_cast<std::int32_t>(0x80070057u) &&
              close(list) == static_cast<std::int32_t>(0x80004005u),
          "failed Reset into a busy allocator leaves the first list closed");
    check(release_device(device) == 4 && release_allocator(allocator) == 0,
          "list and allocator objects independently retain the parent after caller release");
    check(reset(list, allocator, nullptr) == static_cast<std::int32_t>(0x80070057u),
          "a released allocator is rejected before access");
    check(release(second_list) == 0 && reset(list, second_allocator, nullptr) == 0,
          "releasing an open list clears its recording owner for reuse");
    check(release_allocator(second_allocator) == 1 &&
              reset_allocator(second_allocator) == static_cast<std::int32_t>(0x80004005u),
          "the recording lease keeps an allocator alive after caller release");
    check(close(list) == 0 && release(list) == 0,
          "last recording and list leases release the real backend parent");
    if (probe == 3) {
        (void)get_type(list);
        throw std::runtime_error("released list unexpectedly returned a type");
    }
    check(errno == E2BIG && get_last_error() == 89 && crt_error() == 37,
          "list ABI calls preserve native and guest error state");
    device = nullptr;
    check(runtime.graphics_devices().create(&device11_interface, &device, std::nullopt) == 0 && device &&
              release_device(device) == 0,
          "complete child teardown permits another real backend device");
    errno = saved_errno;
}
