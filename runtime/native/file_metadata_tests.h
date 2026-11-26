#ifdef __linux__
#include <fcntl.h>
#include <sys/stat.h>
#endif

void native_file_metadata_checks() {
    using namespace ambrosia;
    using namespace services;
    constexpr std::uint64_t epoch = 116444736000000000;
    check(windows_filetime({0, 0}) == epoch && windows_filetime({-11644473600, 0}) == 0 &&
              windows_filetime({-1, 500000000}) == epoch - 5000000 &&
              windows_filetime({0, 123456789}) == epoch + 1234567 && !windows_filetime({-11644473601, 0}) &&
              !windows_filetime({0, 1000000000}),
          "file timestamps use the 1601 epoch, floor nanoseconds to 100ns, and reject pre-epoch or invalid "
          "fractions without signed overflow");
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    constexpr auto last_second = static_cast<std::int64_t>(maximum / 10000000) - 11644473600;
    constexpr auto last_fraction = static_cast<std::uint32_t>((maximum % 10000000) * 100);
    check(windows_filetime({last_second, last_fraction}) == maximum &&
              !windows_filetime({last_second, last_fraction + 100}) &&
              !windows_filetime({last_second + 1, 0}) &&
              !windows_filetime({std::numeric_limits<std::int64_t>::max(), 0}),
          "file timestamp conversion covers the full unsigned FILETIME endpoint and rejects both fraction "
          "and whole-second overflow before arithmetic");
    const auto root = std::filesystem::path("/work/file-metadata");
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
#ifdef __linux__
    constexpr timespec fixed_times[] = {{946684800, 123456700}, {978307200, 765432100}};
    const auto set_times = ::utimensat(AT_FDCWD, (package / "Item.dat").c_str(), fixed_times, 0);
    check(set_times == 0, "the original scratch file accepts deterministic access/write timestamps");
#endif
    Runtime runtime;
    Scope scope(runtime);
    runtime.files().bind(package, image);
    using Open = void *(AMBROSIA_GUEST_ABI *)(const void *, std::uint32_t, std::uint32_t, const void *,
                                              std::uint32_t, std::uint32_t, void *);
    using Times = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, void *, void *, void *);
    using Size = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, void *);
    using Close = std::int32_t(AMBROSIA_GUEST_ABI *)(void *);
    const auto open = reinterpret_cast<Open>(runtime.resolve(service_symbol("CreateFileW", "kernel32.dll")));
    const auto times = reinterpret_cast<Times>(
        runtime.resolve(service_symbol("GetFileTime", "api-ms-win-core-file-l1-1-0.dll")));
    const auto size =
        reinterpret_cast<Size>(runtime.resolve(service_symbol("GetFileSizeEx", "kernelbase.dll")));
    const auto close =
        reinterpret_cast<Close>(runtime.resolve(service_symbol("CloseHandle", "kernel32.dll")));
    auto *handle = open(u"Q:/Item.dat", 0x80000000u, 1, nullptr, 3, 0, nullptr);
    const auto id = reinterpret_cast<std::uintptr_t>(handle);
    auto object = std::dynamic_pointer_cast<ReadonlyFileObject>(runtime.objects().waitable(id));
    const auto native_times = object->times();
    std::array<std::byte, 32> packed;
    packed.fill(std::byte{0xcc});
    const auto saved_error = errno;
    errno = EDOM;
    crt_error() = 61;
    set_last_error(91);
    const auto actual_times = times(handle, nullptr, packed.data() + 1, packed.data() + 13);
    std::uint64_t access = 0;
    std::uint64_t write = 0;
    std::memcpy(&access, packed.data() + 1, sizeof(access));
    std::memcpy(&write, packed.data() + 13, sizeof(write));
    const bool guards = packed[0] == std::byte{0xcc} && packed[9] == std::byte{0xcc} &&
                        packed[12] == std::byte{0xcc} && packed[21] == std::byte{0xcc};
    check(actual_times == 1 && native_times.error == host::FileLookupError::none &&
              access == epoch + 9466848000000000ull + 1234567 &&
              write == epoch + 9783072000000000ull + 7654321 && guards && errno == EDOM &&
              crt_error() == 61 && get_last_error() == 91,
          "packed optional GetFileTime outputs contain actual deterministic descriptor access/write times "
          "with intact neighboring fields and caller errors");
    std::uint64_t creation = 0xcccccccccccccccc;
    const auto created = times(handle, &creation, nullptr, nullptr);
    check(native_times.creation ? created == 1 && creation == windows_filetime(*native_times.creation)
                                : created == 0 && get_last_error() == 50 && creation == 0xcccccccccccccccc,
          "creation time comes only from a supported native birth timestamp; absent birth metadata fails "
          "explicitly without substituting ctime or modifying output");
    set_last_error(91);
    const auto no_outputs = times(handle, nullptr, nullptr, nullptr);
    const auto actual_size = size(handle, packed.data() + 1);
    std::int64_t bytes = -1;
    std::memcpy(&bytes, packed.data() + 1, sizeof(bytes));
    check(no_outputs == 1 && actual_size == 1 && bytes == 7 && packed[0] == std::byte{0xcc} &&
              packed[9] == std::byte{0xcc} && get_last_error() == 91,
          "null optional time outputs and packed LARGE_INTEGER file sizes query the real owned source "
          "without invented bytes or extra writes");
    std::filesystem::rename(package / "Item.dat", package / "original.dat");
    {
        std::ofstream stream(package / "Item.dat", std::ios::binary);
        stream << "replacement";
    }
    std::uint64_t original_write = 0;
    const auto after_rename = times(handle, nullptr, nullptr, &original_write);
    const auto size_after_rename = size(handle, &bytes);
    check(after_rename == 1 && original_write == write && size_after_rename == 1 && bytes == 7,
          "file times and size remain bound to the original descriptor after pathname replacement");
    auto *metadata = open(u"Q:/Item.dat", 0, 1, nullptr, 3, 0, nullptr);
    std::uint64_t untouched = 0xcccccccccccccccc;
    const auto no_read = times(metadata, nullptr, nullptr, &untouched);
    const auto no_read_error = get_last_error();
    const auto event = runtime.objects().create_event(true, false);
    const auto wrong_type = times(reinterpret_cast<void *>(event.handle), &untouched, nullptr, nullptr);
    const auto type_error = get_last_error();
    const auto closed = close(handle);
    const auto after_close = times(handle, nullptr, &untouched, nullptr);
    check(no_read == 0 && no_read_error == 5 && wrong_type == 0 && type_error == 6 && closed == 1 &&
              after_close == 0 && get_last_error() == 6 && untouched == 0xcccccccccccccccc,
          "GetFileTime rejects metadata-only, wrong-type and closed handles without modifying any output");
    const auto bad_size = size(handle, &untouched);
    const auto bad_size_error = get_last_error();
    const auto missing_size_output = size(metadata, nullptr);
    check(
        bad_size == 0 && bad_size_error == 6 && missing_size_output == 0 && get_last_error() == 87 &&
            untouched == 0xcccccccccccccccc,
        "GetFileSizeEx rejects closed handles and absent output pointers without modifying supplied storage");
    const auto metadata_closed = close(metadata);
    const auto event_closed = runtime.objects().close(event.handle);
    check(metadata_closed == 1 && event_closed == 0,
          "native metadata fixtures close their file/event directory references independently");
    errno = saved_error;
}
