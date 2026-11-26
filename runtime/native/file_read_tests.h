void native_file_read_checks() {
    using namespace ambrosia;
    using namespace services;
    const auto root = std::filesystem::path("/work/file-reads");
    const auto package = root / "assets";
    const auto image = root / "bin" / "app.exe";
    std::filesystem::create_directories(package);
    std::filesystem::create_directories(image.parent_path());
    {
        std::ofstream stream(package / "Item.dat", std::ios::binary);
        stream << "abcdefg";
    }
    {
        std::ofstream stream(package / "Direct.dat", std::ios::binary);
        stream << std::string(4096, 'A') << std::string(4096, 'B');
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
    const auto read = reinterpret_cast<Read>(
        runtime.resolve(service_symbol("ReadFile", "api-ms-win-core-file-l1-1-0.dll")));
    const auto close =
        reinterpret_cast<Close>(runtime.resolve(service_symbol("CloseHandle", "kernel32.dll")));
    auto *handle = open(u"Q:/Item.dat", 0x80000000u, 1, nullptr, 3, 0, nullptr);
    auto *independent = open(u"Q:/Item.dat", 0x80000000u, 1, nullptr, 3, 0, nullptr);
    auto *metadata = open(u"Q:/Item.dat", 0, 1, nullptr, 3, 0, nullptr);
    std::array<std::byte, 16> buffer;
    buffer.fill(std::byte{0xcc});
    std::array<std::byte, 8> packed_count;
    packed_count.fill(std::byte{0xcc});
    const auto saved_error = errno;
    errno = EDOM;
    crt_error() = 61;
    set_last_error(91);
    const auto first = read(handle, buffer.data() + 1, 3, packed_count.data() + 1, nullptr);
    std::uint32_t count = 0;
    std::memcpy(&count, packed_count.data() + 1, sizeof(count));
    check(first == 1 && count == 3 && std::memcmp(buffer.data() + 1, "abc", 3) == 0 &&
              buffer[0] == std::byte{0xcc} && buffer[4] == std::byte{0xcc} &&
              packed_count[0] == std::byte{0xcc} && packed_count[5] == std::byte{0xcc} && errno == EDOM &&
              crt_error() == 61 && get_last_error() == 91,
          "synchronous ReadFile copies actual original bytes and a packed four-byte count without modifying "
          "guards or caller errors");
    buffer.fill(std::byte{0xcc});
    const auto short_read = read(handle, buffer.data() + 1, 8, &count, nullptr);
    check(short_read == 1 && count == 4 && std::memcmp(buffer.data() + 1, "defg", 4) == 0 &&
              buffer[0] == std::byte{0xcc} && buffer[5] == std::byte{0xcc},
          "each synchronous source advances its cursor by actual bytes, including a short read at EOF");
    buffer.fill(std::byte{0xcc});
    count = 99;
    const auto eof = read(handle, buffer.data(), 8, &count, nullptr);
    const auto zero = read(handle, nullptr, 0, &count, nullptr);
    check(eof == 1 && zero == 1 && count == 0 && buffer[0] == std::byte{0xcc},
          "ordinary synchronous EOF and zero-byte reads succeed with zero bytes and no fabricated payload");
    count = 99;
    const auto no_count = read(independent, buffer.data(), 1, nullptr, nullptr);
    const auto no_count_error = get_last_error();
    const auto no_buffer = read(independent, nullptr, 1, &count, nullptr);
    const auto no_buffer_error = get_last_error();
    const auto oversized =
        read(independent, buffer.data(), host::ReadonlyFile::read_limit + 1, &count, nullptr);
    check(no_count == 0 && no_count_error == 87 && no_buffer == 0 && no_buffer_error == 87 &&
              oversized == 0 && get_last_error() == 8 && count == 0 && buffer[0] == std::byte{0xcc},
          "missing mandatory outputs/buffers and over-budget reads fail before payload access and zero the "
          "provided byte count");
    std::array<std::byte, 32> overlapped;
    overlapped.fill(std::byte{0xcc});
    const auto async = read(independent, buffer.data(), 1, &count, overlapped.data());
    const auto async_error = get_last_error();
    const auto no_access = read(metadata, buffer.data(), 1, &count, nullptr);
    const auto access_error = get_last_error();
    const auto event = runtime.objects().create_event(true, false);
    const auto wrong_type = read(reinterpret_cast<void *>(event.handle), buffer.data(), 1, &count, nullptr);
    check(
        async == 0 && async_error == 50 && overlapped[0] == std::byte{0xcc} && no_access == 0 &&
            access_error == 5 && wrong_type == 0 && get_last_error() == 6 && count == 0 &&
            buffer[0] == std::byte{0xcc},
        "unsupported OVERLAPPED, metadata-only and wrong-type handles cannot read bytes or fake completion");
    const auto independent_read = read(independent, buffer.data(), 3, &count, nullptr);
    const auto wait = runtime.objects().wait(reinterpret_cast<std::uintptr_t>(independent), 0);
    check(independent_read == 1 && count == 3 && std::memcmp(buffer.data(), "abc", 3) == 0 &&
              wait.result == wait_object_0 && !wait.error,
          "separate file objects own independent cursors; failed calls do not advance them and completed "
          "reads leave the file signaled");
    auto *direct = open(u"Q:/Direct.dat", 0x80000000u, 1, nullptr, 3, 0x20000000u, nullptr);
    alignas(4096) std::array<std::byte, 8192> aligned;
    aligned.fill(std::byte{0xcc});
    const auto direct_read = read(direct, aligned.data(), 4096, &count, nullptr);
    const auto direct_error = get_last_error();
    if (direct_read) {
        const auto first_count = count;
        const bool first_payload = std::all_of(aligned.begin(), aligned.begin() + 4096,
                                               [](auto byte) { return byte == std::byte{'A'}; });
        const auto misaligned = read(direct, aligned.data() + 1, 4096, &count, nullptr);
        const auto alignment_error = get_last_error();
        const auto second_direct = read(direct, aligned.data() + 4096, 4096, &count, nullptr);
        check(first_count == 4096 && first_payload && misaligned == 0 && alignment_error == 87 &&
                  second_direct == 1 && count == 4096 &&
                  std::all_of(aligned.begin() + 4096, aligned.end(),
                              [](auto byte) { return byte == std::byte{'B'}; }),
              "real direct reads validate kernel-reported buffer alignment and retain their cursor across "
              "alignment failure before reading the second original block");
        std::puts("DIRECT_READ_FIXTURE native-alignment-and-payload-verified");
    } else {
        check(direct_error == 50 && count == 0 && aligned[0] == std::byte{0xcc},
              "a filesystem without authoritative direct-alignment metadata fails explicitly without "
              "buffered fallback or payload writes");
        std::puts("DIRECT_READ_FIXTURE alignment-metadata-unavailable; direct-payload-coverage-skipped");
    }
    const auto closed = close(independent);
    count = 99;
    const auto after_close = read(independent, buffer.data(), 1, &count, nullptr);
    check(closed == 1 && after_close == 0 && get_last_error() == 6 && count == 0,
          "closed read handles cannot reach retained native objects and clear the count before failing");
    const auto closed_handle = close(handle);
    const auto closed_metadata = close(metadata);
    const auto closed_direct = close(direct);
    const auto closed_event = runtime.objects().close(event.handle);
    check(closed_handle == 1 && closed_metadata == 1 && closed_direct == 1 && closed_event == 0,
          "original read fixtures release their file and event references independently");
    errno = saved_error;
}
