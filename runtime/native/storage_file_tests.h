#ifdef __linux__
#include <sys/stat.h>
#endif

void storage_file_checks() {
    using namespace ambrosia;
    using namespace services;
    const auto root = std::filesystem::path("/work/storage-files");
    const auto package = root / "assets";
    const auto image = root / "bin" / "app.exe";
    std::filesystem::create_directories(package / "Folder");
    std::filesystem::create_directories(image.parent_path());
    const auto write = [](const auto &path, std::string_view contents) {
        std::ofstream stream(path, std::ios::binary);
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!stream) {
            throw std::runtime_error("cannot write original storage fixture");
        }
    };
    write(package / "Folder" / "Item.dat", "fixture");
    write(image, "original fixture");
    std::filesystem::create_symlink("Folder/Item.dat", package / "link");
    Runtime runtime;
    Runtime independent;
    Scope scope(runtime);
    runtime.files().bind(package, image);
    void *factory = nullptr;
    const auto factory_result = runtime.storage_factory().query(&storage_factory_x2_id, &factory);
    const auto *table = *static_cast<const std::uintptr_t *const *>(factory);
    using Open = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, const void *, const void *, void *);
    using Query = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, const void *, void *);
    using Reference = std::uint32_t(AMBROSIA_GUEST_ABI *)(void *);
    using Close = void(AMBROSIA_GUEST_ABI *)(void *);
    const auto open = reinterpret_cast<Open>(table[4]);
    const auto descriptor_count = [] {
        std::size_t count = 0;
        for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd")) {
            (void)entry;
            ++count;
        }
        return count;
    };
    const auto saved_error = errno;
    errno = EDOM;
    crt_error() = 61;
    set_last_error(91);
    const auto before = descriptor_count();
    void *object = nullptr;
    const auto opened = open(factory, u"Q:/folder/ITEM.dat", &storage_file_x_id, &object);
    check(factory_result == 0 && opened == 0 && object && descriptor_count() == before + 1 &&
              runtime.storage_factory().inspect_file(object).metadata.bytes == 7 && errno == EDOM &&
              crt_error() == 61 && get_last_error() == 91,
          "console storage OpenFile's fifth Microsoft ABI argument returns an owned real read-only file "
          "with actual metadata and preserved native/CRT/Windows errors");
    const auto *methods = *static_cast<const std::uintptr_t *const *>(object);
    const auto query = reinterpret_cast<Query>(methods[0]);
    const auto add_reference = reinterpret_cast<Reference>(methods[1]);
    const auto release = reinterpret_cast<Reference>(methods[2]);
    const auto close = reinterpret_cast<Close>(methods[3]);
    void *unknown = nullptr;
    const auto queried = query(object, &unknown_interface_id, &unknown);
    const auto queried_release = release(unknown);
    const auto added = add_reference(object);
    const auto retained = release(object);
    check(queried == 0 && unknown == object && queried_release == 2 && added == 3 && retained == 2 &&
              descriptor_count() == before + 1,
          "file QueryInterface/AddRef/Release share the same identity and retain the descriptor until the "
          "last guest reference");
    std::filesystem::rename(package / "Folder" / "Item.dat", package / "Folder" / "original.dat");
    write(package / "Folder" / "Item.dat", "replacement");
    const auto still_original = runtime.storage_factory().inspect_file(object);
    check(still_original.error == host::FileLookupError::none && still_original.metadata.bytes == 7,
          "an open storage file retains its original inode when the pathname is replaced instead of "
          "reopening a potentially different input");
    const auto final_release = release(object);
    check(final_release == 1 && descriptor_count() == before &&
              runtime.storage_factory().inspect_file(object).error == host::FileLookupError::invalid,
          "final guest release retains only a tombstone and closes the actual native descriptor");
    void *next = nullptr;
    const auto reopened = open(factory, u"Q:/Folder/Item.dat", &storage_file_x_id, &next);
    const auto next_metadata = runtime.storage_factory().inspect_file(next);
    close(next);
    close(next);
    const auto closed = runtime.storage_factory().inspect_file(next);
    const auto closed_release = release(next);
    check(reopened == 0 && next != object && next_metadata.metadata.bytes == 11 &&
              closed.error == host::FileLookupError::invalid && closed_release == 1 &&
              descriptor_count() == before,
          "later opens receive a fresh object and real replacement metadata; repeated Close and final "
          "release are safe and cannot reuse a stale file pointer");
    const Guid absent{};
    void *failed = reinterpret_cast<void *>(1);
    const auto bad_id = open(factory, u"Q:/Folder/Item.dat", &absent, &failed);
    const bool cleared_bad_id = !failed;
    failed = reinterpret_cast<void *>(1);
    const auto missing = open(factory, u"Q:/absent", &storage_file_x_id, &failed);
    const bool cleared_missing = !failed;
    failed = reinterpret_cast<void *>(1);
    const auto missing_parent = open(factory, u"Q:/absent/item", &storage_file_x_id, &failed);
    check(bad_id == no_interface && cleared_bad_id && missing == static_cast<std::int32_t>(0x80070002u) &&
              cleared_missing && missing_parent == static_cast<std::int32_t>(0x80070003u) && !failed &&
              descriptor_count() == before,
          "unsupported file identities and missing files/parents clear output with distinct failure "
          "HRESULTs and no descriptor leaks");
    failed = reinterpret_cast<void *>(1);
    const auto null_path = open(factory, nullptr, &storage_file_x_id, &failed);
    const bool cleared_null_path = !failed;
    const auto null_output = open(factory, u"Q:/Folder/Item.dat", &storage_file_x_id, nullptr);
    const char16_t malformed[] = {0xd800, 0};
    const auto bad_utf16 = open(factory, malformed, &storage_file_x_id, &failed);
    check(null_path == pointer_error && cleared_null_path && null_output == pointer_error &&
              bad_utf16 == static_cast<std::int32_t>(0x8007007bu) && !failed,
          "invalid pointers and malformed UTF16 cannot publish a partially initialized storage file");
#ifdef __linux__
    const auto fifo = package / "pipe";
    const auto created_fifo = ::mkfifo(fifo.c_str(), 0600);
    const auto fifo_result = open(factory, u"Q:/pipe", &storage_file_x_id, &failed);
    check(created_fifo == 0 && fifo_result == static_cast<std::int32_t>(0x80070032u) && !failed &&
              descriptor_count() == before,
          "a nonblocking open rejects and closes a FIFO without reading payloads or waiting for a peer");
#endif
    const auto directory = open(factory, u"Q:/Folder", &storage_file_x_id, &failed);
    const auto traversal = open(factory, u"Q:/../outside", &storage_file_x_id, &failed);
    const auto link = open(factory, u"Q:/link", &storage_file_x_id, &failed);
    check(directory == static_cast<std::int32_t>(0x80070005u) && traversal == directory &&
              link == directory && !failed && descriptor_count() == before,
          "the storage factory reuses descriptor confinement and cannot open directories, traversal or "
          "symlinks as source files");
    {
        auto direct = runtime.files().open("Folder/Item.dat");
        const auto direct_metadata = direct.file->inspect();
        check(!direct.error && direct_metadata.metadata.bytes == 11 && descriptor_count() == before + 1,
              "the portable namespace exposes an owned host file rather than a fabricated path handle");
    }
    check(descriptor_count() == before, "host read-only file destruction closes its descriptor");
    std::vector<void *> held;
    bool budget = true;
    for (unsigned index = 0; index < 32; ++index) {
        void *held_file = nullptr;
        const auto result = open(factory, u"Q:/Folder/Item.dat", &storage_file_x_id, &held_file);
        budget = budget && result == 0 && held_file;
        if (held_file) {
            held.push_back(held_file);
        }
    }
    const auto limited = open(factory, u"Q:/Folder/Item.dat", &storage_file_x_id, &failed);
    for (auto *held_file : held) {
        (void)release(held_file);
    }
    check(budget && limited == gdk_out_of_memory && !failed && descriptor_count() == before &&
              independent.storage_factory().references() == 1,
          "the per-runtime 32-file budget fails explicitly and released files return their native resource "
          "budget without affecting independent runtimes");
    void *packed_file = nullptr;
    std::array<std::byte, 64> packed;
    packed.fill(std::byte{0xcc});
    constexpr char16_t name[] = u"Q:/Folder/Item.dat";
    std::memcpy(packed.data() + 1, name, sizeof(name));
    const auto packed_result = open(factory, packed.data() + 1, &unknown_interface_id, &packed_file);
    const auto packed_release = release(packed_file);
    check(packed_result == 0 && packed_release == 1 && packed[0] == std::byte{0xcc} &&
              packed[sizeof(name) + 1] == std::byte{0xcc} && descriptor_count() == before && errno == EDOM &&
              crt_error() == 61 && get_last_error() == 91,
          "packed UTF16 and IUnknown creation use the same real file ownership without modifying the "
          "filename or caller errors");
    errno = saved_error;
}
