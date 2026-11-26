void native_file_handle_checks() {
    using namespace ambrosia;
    using namespace services;
    const auto root = std::filesystem::path("/work/file-handles");
    const auto package = root / "assets";
    const auto image = root / "bin" / "app.exe";
    std::filesystem::create_directories(package);
    std::filesystem::create_directories(image.parent_path());
    {
        std::ofstream stream(package / "Item.dat", std::ios::binary);
        stream << "fixture";
    }
    {
        std::ofstream stream(image, std::ios::binary);
        stream << "original fixture";
    }
    std::filesystem::create_hard_link(package / "Item.dat", package / "alias.dat");
    std::filesystem::create_symlink("Item.dat", package / "link.dat");
    Runtime runtime;
    Scope scope(runtime);
    runtime.files().bind(package, image);
    using OpenWide = void *(AMBROSIA_GUEST_ABI *)(const void *, std::uint32_t, std::uint32_t, const void *,
                                                  std::uint32_t, std::uint32_t, void *);
    using OpenBytes = void *(AMBROSIA_GUEST_ABI *)(const char *, std::uint32_t, std::uint32_t, const void *,
                                                   std::uint32_t, std::uint32_t, void *);
    using Close = std::int32_t(AMBROSIA_GUEST_ABI *)(void *);
    const auto wide = reinterpret_cast<OpenWide>(
        runtime.resolve(service_symbol("CreateFileW", "api-ms-win-core-file-l1-1-0.dll")));
    const auto bytes =
        reinterpret_cast<OpenBytes>(runtime.resolve(service_symbol("CreateFileA", "kernel32.dll")));
    const auto close =
        reinterpret_cast<Close>(runtime.resolve(service_symbol("CloseHandle", "kernelbase.dll")));
    const auto invalid = reinterpret_cast<void *>(~std::uintptr_t{0});
    const auto saved_error = errno;
    errno = EDOM;
    crt_error() = 61;
    set_last_error(91);
    auto *handle = wide(u"Q:/Item.dat", 0x80000000u, 1, nullptr, 3, 0x20000000u, nullptr);
    const auto id = reinterpret_cast<std::uintptr_t>(handle);
    auto object = std::dynamic_pointer_cast<ReadonlyFileObject>(runtime.objects().waitable(id));
    check(handle != invalid && handle && object && object->read_data() &&
              object->inspect().metadata.bytes == 7 && object->inspect().metadata.unbuffered &&
              runtime.objects().reference(id).access == (0x80000000u | object_synchronize) && errno == EDOM &&
              crt_error() == 61 && get_last_error() == 91,
          "seven-argument CreateFileW returns an opaque readable handle backed by an actual O_DIRECT "
          "descriptor with real metadata and preserved caller errors");
    const auto polled = runtime.objects().wait(id, 0);
    const auto wrong_type = runtime.objects().set_event(id);
    check(polled.result == wait_object_0 && !polled.error && wrong_type == 6,
          "a synchronous file without pending I/O is signaled and cannot be confused with an event");
    auto *second = bytes("Q:/alias.dat", 0x80000000u, 1, nullptr, 3, 0x80, nullptr);
    auto *exclusive = wide(u"Q:/alias.dat", 0x80000000u, 0, nullptr, 3, 0, nullptr);
    const auto sharing_error = get_last_error();
    auto *metadata_only = wide(u"Q:/Item.dat", 0, 0, nullptr, 3, 0, nullptr);
    const auto metadata_id = reinterpret_cast<std::uintptr_t>(metadata_only);
    const auto metadata_object =
        std::dynamic_pointer_cast<ReadonlyFileObject>(runtime.objects().waitable(metadata_id));
    check(second != invalid && second != handle && exclusive == invalid && sharing_error == 32 &&
              metadata_only != invalid && metadata_object && !metadata_object->read_data(),
          "hard-link aliases share inode-based symmetric read-sharing rules while metadata-only access "
          "does not participate in data-sharing restrictions");
    const auto closed_second = close(second);
    const auto closed_first = close(handle);
    auto *still_blocked = wide(u"Q:/alias.dat", 0x80000000u, 0, nullptr, 3, 0, nullptr);
    const auto retained_error = get_last_error();
    object.reset();
    auto *now_exclusive = wide(u"Q:/alias.dat", 0x80000000u, 0, nullptr, 3, 0, nullptr);
    check(closed_second == 1 && closed_first == 1 && still_blocked == invalid && retained_error == 32 &&
              now_exclusive != invalid && !runtime.objects().waitable(id),
          "CloseHandle drops the directory reference while an in-flight native owner retains the source "
          "and its sharing lease until that last owner releases it");
    auto blocked_storage = runtime.files().open("Item.dat");
    const auto close_exclusive = close(now_exclusive);
    auto allowed_storage = runtime.files().open("Item.dat");
    check(blocked_storage.error == 32 && !blocked_storage.file && close_exclusive == 1 &&
              !allowed_storage.error && allowed_storage.file,
          "DirectStorage and Win32 opens obey one shared read-only source budget and sharing policy");
    allowed_storage.file.reset();
    const auto closed_metadata = close(metadata_only);
    auto *reopened = wide(u"Q:/Item.dat", 0x80000000u, 1, nullptr, 3, 0, nullptr);
    const auto closed_reopened = close(reopened);
    const auto repeated_close = close(reopened);
    check(closed_metadata == 1 && reopened != invalid && reopened != handle && closed_reopened == 1 &&
              repeated_close == 0 && get_last_error() == 6,
          "closed file IDs are never reused and repeated CloseHandle reports ERROR_INVALID_HANDLE");
    auto *write_request = wide(u"Q:/Item.dat", 0x40000000u, 1, nullptr, 3, 0, nullptr);
    const auto write_error = get_last_error();
    auto *creation = wide(u"Q:/new.dat", 0x80000000u, 1, nullptr, 2, 0, nullptr);
    const auto creation_error = get_last_error();
    auto *bad_disposition = wide(u"Q:/Item.dat", 0x80000000u, 1, nullptr, 0, 0, nullptr);
    const auto disposition_error = get_last_error();
    auto *bad_sharing = wide(u"Q:/Item.dat", 0x80000000u, 8, nullptr, 3, 0, nullptr);
    check(write_request == invalid && write_error == 5 && creation == invalid && creation_error == 5 &&
              bad_disposition == invalid && disposition_error == 87 && bad_sharing == invalid &&
              get_last_error() == 87 && !std::filesystem::exists(package / "new.dat"),
          "write/create requests cannot mutate read-only assets; invalid disposition/share masks return "
          "Windows errors rather than a fake handle");
    auto *async = wide(u"Q:/Item.dat", 0x80000000u, 1, nullptr, 3, 0x40000000u, nullptr);
    const auto async_error = get_last_error();
    auto *security = wide(u"Q:/Item.dat", 0x80000000u, 1, &root, 3, 0, nullptr);
    const auto security_error = get_last_error();
    auto *template_file = wide(u"Q:/Item.dat", 0x80000000u, 1, nullptr, 3, 0, reinterpret_cast<void *>(4));
    check(async == invalid && async_error == 50 && security == invalid && security_error == 50 &&
              template_file == invalid && get_last_error() == 50,
          "unimplemented asynchronous, security and template semantics fail explicitly");
    auto *missing = wide(u"Q:/missing.dat", 0x80000000u, 1, nullptr, 3, 0, nullptr);
    const auto missing_error = get_last_error();
    auto *escaped = wide(u"Q:/../outside", 0x80000000u, 1, nullptr, 3, 0, nullptr);
    const auto escape_error = get_last_error();
    auto *linked = wide(u"Q:/link.dat", 0x80000000u, 1, nullptr, 3, 0, nullptr);
    check(missing == invalid && missing_error == 2 && escaped == invalid && escape_error == 5 &&
              linked == invalid && get_last_error() == 5,
          "native file handles preserve missing-file errors and cannot bypass path or symlink confinement");
    errno = EDOM;
    crt_error() = 61;
    auto *null_path = wide(nullptr, 0x80000000u, 1, nullptr, 3, 0, nullptr);
    const auto null_error = get_last_error();
    const char16_t malformed[] = {0xd800, 0};
    auto *bad_utf16 = wide(malformed, 0x80000000u, 1, nullptr, 3, 0, nullptr);
    check(null_path == invalid && null_error == 87 && bad_utf16 == invalid && get_last_error() == 123 &&
              errno == EDOM && crt_error() == 61,
          "null and malformed UTF16 inputs cannot expose a handle and preserve native/CRT errors");
    std::array<std::byte, 48> packed;
    packed.fill(std::byte{0xcc});
    constexpr char16_t name[] = u"Q:/Item.dat";
    std::memcpy(packed.data() + 1, name, sizeof(name));
    set_last_error(91);
    auto *packed_handle = wide(packed.data() + 1, 0x80000000u, 1, nullptr, 3, 0, nullptr);
    const auto packed_error = get_last_error();
    const auto packed_closed = close(packed_handle);
    check(packed_handle != invalid && packed_closed == 1 && packed_error == 91 &&
              packed[0] == std::byte{0xcc} && packed[sizeof(name) + 1] == std::byte{0xcc},
          "packed UTF16 filenames are read without modifying guards or successful-open error state");
    errno = saved_error;
}
