void synchronous_file_offset_checks() {
    using namespace ambrosia;
    using namespace services;
    const auto root = std::filesystem::path("/work/file-offsets");
    const auto package = root / "assets";
    const auto image = root / "bin" / "app.exe";
    std::filesystem::create_directories(package);
    std::filesystem::create_directories(image.parent_path());
    {
        std::ofstream stream(package / "Item.dat", std::ios::binary);
        stream << "abcdefg";
    }
    {
        std::ofstream stream(image, std::ios::binary);
        stream << "original fixture";
    }
    Runtime runtime;
    Scope scope(runtime);
    runtime.files().bind(package, image);
    using Open = void *(AMBROSIA_GUEST_ABI *)(const void *, std::uint32_t, std::uint32_t, const void *,
                                              std::uint32_t, std::uint32_t, void *);
    using Read = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, void *, std::uint32_t, void *, void *);
    using Close = std::int32_t(AMBROSIA_GUEST_ABI *)(void *);
    const auto open = reinterpret_cast<Open>(runtime.resolve(service_symbol("CreateFileW", "kernel32.dll")));
    const auto read = reinterpret_cast<Read>(runtime.resolve(service_symbol("ReadFile", "kernel32.dll")));
    const auto close =
        reinterpret_cast<Close>(runtime.resolve(service_symbol("CloseHandle", "kernel32.dll")));
    auto *handle = open(u"Q:/Item.dat", 0x80000000u, 1, nullptr, 3, 0, nullptr);
    std::array<std::byte, 34> packed;
    packed.fill(std::byte{0xcc});
    std::memset(packed.data() + 1, 0, 32);
    auto *overlapped = packed.data() + 1;
    const auto store = [&](std::size_t position, std::uint64_t value) {
        std::memcpy(overlapped + position, &value, sizeof(value));
    };
    const auto load = [&](std::size_t position) {
        std::uint64_t value;
        std::memcpy(&value, overlapped + position, sizeof(value));
        return value;
    };
    std::array<std::byte, 16> buffer;
    buffer.fill(std::byte{0xcc});
    store(16, 2);
    std::uint32_t count = 99;
    const auto saved_error = errno;
    errno = EDOM;
    crt_error() = 61;
    set_last_error(91);
    const auto offset_read = read(handle, buffer.data() + 1, 3, &count, overlapped);
    check(offset_read == 1 && count == 3 && std::memcmp(buffer.data() + 1, "cde", 3) == 0 && load(0) == 0 &&
              load(8) == 3 && load(16) == 5 && load(24) == 0 && packed.front() == std::byte{0xcc} &&
              packed.back() == std::byte{0xcc} && buffer[0] == std::byte{0xcc} &&
              buffer[4] == std::byte{0xcc} && errno == EDOM && crt_error() == 61 && get_last_error() == 91,
          "a packed OVERLAPPED on a synchronous source reads the actual requested offset and publishes "
          "completed status, byte count and next offset with intact caller fields/errors");
    const auto implicit = read(handle, buffer.data(), 8, &count, nullptr);
    check(implicit == 1 && count == 2 && std::memcmp(buffer.data(), "fg", 2) == 0,
          "a successful explicit-offset read also updates the synchronous file cursor for following "
          "implicit reads");
    store(16, 1);
    const auto optional_count = read(handle, buffer.data(), 2, nullptr, overlapped);
    check(optional_count == 1 && load(0) == 0 && load(8) == 2 && load(16) == 3 &&
              std::memcmp(buffer.data(), "bc", 2) == 0,
          "OVERLAPPED InternalHigh supplies the actual byte count when the optional count pointer is null");
    store(16, std::numeric_limits<std::uint64_t>::max());
    buffer.fill(std::byte{0xcc});
    const auto invalid_offset = read(handle, buffer.data(), 1, &count, overlapped);
    const auto invalid_error = get_last_error();
    const auto invalid_status = load(0);
    const auto invalid_count = load(8);
    const auto after_invalid = read(handle, buffer.data(), 2, &count, nullptr);
    check(
        invalid_offset == 0 && invalid_error == 87 && invalid_status == 0xc000000du && invalid_count == 0 &&
            after_invalid == 1 && count == 2 && std::memcmp(buffer.data(), "de", 2) == 0,
        "an unrepresentable explicit offset publishes failure without advancing the established file cursor");
    store(16, 7);
    buffer.fill(std::byte{0xcc});
    const auto eof = read(handle, buffer.data(), 1, &count, overlapped);
    check(eof == 0 && get_last_error() == 38 && count == 0 && load(0) == 0xc0000011u && load(8) == 0 &&
              load(16) == 7 && buffer[0] == std::byte{0xcc},
          "explicit-offset EOF reports ERROR_HANDLE_EOF and completed end-of-file status instead of a "
          "retryable unsupported error or invented bytes");
    store(16, 0);
    set_last_error(91);
    const auto zero = read(handle, nullptr, 0, nullptr, overlapped);
    const auto after_zero = read(handle, buffer.data(), 1, &count, nullptr);
    check(zero == 1 && load(0) == 0 && load(8) == 0 && load(16) == 0 && after_zero == 1 && count == 1 &&
              buffer[0] == std::byte{'a'} && get_last_error() == 91,
          "a zero-byte explicit operation completes synchronously with its selected position and no payload");
    store(0, 0xcccccccccccccccc);
    store(8, 0xcccccccccccccccc);
    store(24, 4);
    const auto event_mode = read(handle, buffer.data(), 1, &count, overlapped);
    check(event_mode == 0 && get_last_error() == 50 && count == 0 && load(0) == 0xcccccccccccccccc &&
              load(8) == 0xcccccccccccccccc,
          "unimplemented event completion remains explicit and cannot publish a false completed operation");
    const auto closed = close(handle);
    check(closed == 1, "explicit-offset source fixtures close their real handle");
    errno = saved_error;
}
