void storage_directory_checks() {
    using namespace ambrosia;
    using namespace services;
    const auto root = std::filesystem::path("/work/storage-directories");
    const auto assets = root / "assets";
    const auto image = root / "bin" / "original.exe";
    const auto state = root / "state";
    std::filesystem::create_directories(assets);
    std::filesystem::create_directories(image.parent_path());
    std::filesystem::create_directories(state);
    {
        std::ofstream file(assets / "original.txt");
        file << "original";
    }
    Runtime runtime;
    Scope scope(runtime);
    runtime.files().bind(assets, image);
    runtime.files().bind_additional_native_root(state);
    runtime.persistent_storage().bind(state);
    using Create = std::uint32_t(AMBROSIA_GUEST_ABI *)(const void *, const void *);
    const auto address = runtime.resolve(service_symbol("CreateDirectoryW", "kernel32.dll"));
    check(address && address == runtime.resolve(service_symbol("CreateDirectoryW", "kernelbase.dll")) &&
              address ==
                  runtime.resolve(service_symbol("CreateDirectoryW", "api-ms-win-core-file-l1-1-0.dll")),
          "CreateDirectoryW aliases share the real storage-confined Windows entrypoint");
    if (!address) {
        return;
    }
    const auto create = reinterpret_cast<Create>(address);
    const auto call = [&](const std::filesystem::path &path) {
        return create(path.u16string().c_str(), nullptr);
    };
    const auto saved_errno = errno;
    errno = E2BIG;
    crt_error() = 31;
    set_last_error(87);
    const auto created = call(state / "Child");
    const auto preserved = errno == E2BIG && crt_error() == 31 && get_last_error() == 87;
    check(created == 1 && preserved && std::filesystem::is_directory(state / "Child"),
          "directory creation makes a real state directory and preserves errors on success");
    check(call(state / "Child") == 0 && get_last_error() == 183 && call(state / "child") == 0 &&
              get_last_error() == 183 && !std::filesystem::exists(state / "child"),
          "existing directory names, including ASCII case aliases, return ERROR_ALREADY_EXISTS");
    check(call(state / "cHiLd" / "Nested") == 1 && std::filesystem::is_directory(state / "Child" / "Nested"),
          "confined writable parents reuse the read layer's bounded ASCII case resolution");
    check(call(state / "absent" / "leaf") == 0 && get_last_error() == 3 &&
              !std::filesystem::exists(state / "absent"),
          "CreateDirectory creates only the final component and reports a missing intermediate parent");
    check(call(state) == 0 && get_last_error() == 183,
          "the existing storage root is not reported as a newly created directory");
    {
        std::ofstream file(state / "existing.txt");
        file << "fixture";
    }
    check(call(state / "existing.txt") == 0 && get_last_error() == 183 &&
              std::filesystem::file_size(state / "existing.txt") == 7,
          "an existing regular file blocks directory creation without changing its contents");
    check(call(assets / "forbidden") == 0 && get_last_error() == 5 &&
              call(state / ".." / "assets" / "forbidden") == 0 && get_last_error() == 5 &&
              create(u"Q:/forbidden", nullptr) == 0 && get_last_error() == 5 &&
              !std::filesystem::exists(assets / "forbidden"),
          "asset-native paths, normalized escapes and DOS asset aliases cannot acquire write access");
    std::filesystem::create_directory_symlink(assets, state / "outside-link");
    std::filesystem::create_directory_symlink(state / "Child", state / "inside-link");
    check(call(state / "outside-link" / "forbidden") == 0 && get_last_error() == 5 &&
              call(state / "inside-link" / "forbidden") == 0 && get_last_error() == 5 &&
              !std::filesystem::exists(assets / "forbidden") &&
              !std::filesystem::exists(state / "Child" / "forbidden"),
          "descriptor-relative directory creation rejects both escaping and internal symlink parents");
    std::filesystem::create_symlink(assets / "original.txt", state / "final-link");
    check(call(state / "final-link") == 0 && get_last_error() == 5 &&
              std::filesystem::file_size(assets / "original.txt") == 8,
          "a planted final symlink is refused and its target remains untouched");
    const auto unicode_path = state / std::filesystem::path(u8"café");
    check(call(unicode_path) == 1 && std::filesystem::is_directory(unicode_path),
          "valid UTF16 directory names create the corresponding real UTF8 filesystem name");
    const auto packed_path = (state / "packed").u16string();
    std::vector<std::byte> packed((packed_path.size() + 1) * sizeof(char16_t) + 2, std::byte{0xcc});
    std::memcpy(packed.data() + 1, packed_path.c_str(), (packed_path.size() + 1) * sizeof(char16_t));
    check(create(packed.data() + 1, nullptr) == 1 && std::filesystem::is_directory(state / "packed") &&
              packed.front() == std::byte{0xcc} && packed.back() == std::byte{0xcc},
          "unaligned Windows UTF16 input is decoded without altering source guards");
    const char16_t invalid[] = {char16_t(0xd800), 0};
    check(create(nullptr, nullptr) == 0 && get_last_error() == 87 && create(invalid, nullptr) == 0 &&
              get_last_error() == 123,
          "null and malformed directory path inputs return explicit Windows parameter/name errors");
    const std::u16string oversized(32768, u'x');
    check(create(oversized.c_str(), nullptr) == 0 && get_last_error() == 206,
          "directory paths exceeding the existing UTF16 read budget fail before filesystem mutation");
    const auto state_identity =
        runtime.files().resolve(state.generic_string()).directory->inspect({}).metadata;
    const auto asset_identity =
        runtime.files().resolve(assets.generic_string()).directory->inspect({}).metadata;
    host::LocalStorageDirectory host_directory(state);
    const std::array<std::string, 1> name{"direct"};
    check(host_directory.create_directory(asset_identity, name) == host::DirectoryCreateError::denied &&
              !std::filesystem::exists(state / "direct") &&
              host_directory.create_directory(state_identity, std::array<std::string, 1>{".."}) ==
                  host::DirectoryCreateError::invalid &&
              host_directory.create_directory(state_identity, std::vector<std::string>(1025, "x")) ==
                  host::DirectoryCreateError::too_long,
          "the host independently enforces root identity, component validation and traversal budgets");
    errno = saved_errno;
}
