void private_memory_option_checks() {
    std::ostringstream output, errors;
    bool invalid = true;
    for (const auto *value :
         {"", "0", "-1", "+1", "8193", "999999999999999999999999", "2MiB", "2.0", " 2", "0x2"}) {
        const std::vector<std::filesystem::path> arguments{"--trace-startup", "unused",
                                                           "--private-memory-mib", value};
        output.str("");
        errors.str("");
        invalid = invalid && ambrosia::run_native_command(arguments, output, errors) == 2 &&
                  errors.str().find("private-memory-mib") != std::string::npos;
    }
    check(invalid,
          "private-memory option rejects missing digits, signs, overflow, suffixes and out-of-range values");
    const std::vector<std::filesystem::path> missing{"--trace-startup", "unused", "--private-memory-mib"};
    output.str("");
    errors.str("");
    check(ambrosia::run_native_command(missing, output, errors) == 2 &&
              errors.str().find("requires a value") != std::string::npos,
          "private-memory option requires its own numeric argument");
    const std::vector<std::filesystem::path> duplicate{
        "--trace-startup", "unused", "--private-memory-mib", "1", "--private-memory-mib", "2"};
    output.str("");
    errors.str("");
    check(ambrosia::run_native_command(duplicate, output, errors) == 2 &&
              errors.str().find("duplicate private-memory") != std::string::npos,
          "duplicate private-memory limits fail instead of silently choosing one");
    const std::vector<std::filesystem::path> inspect{"--inspect", "unused", "--private-memory-mib", "1"};
    output.str("");
    errors.str("");
    check(ambrosia::run_native_command(inspect, output, errors) == 2 &&
              errors.str().find("requires trace-startup") != std::string::npos,
          "private-memory option cannot alter read-only inspection mode");
    const std::vector<std::filesystem::path> fixed{"--trace-startup", "unused", "--boot-memory",
                                                   "--private-memory-mib", "8192"};
    output.str("");
    errors.str("");
    check(ambrosia::run_native_command(fixed, output, errors) == 2 &&
              errors.str().find("conflicts with the fixed") != std::string::npos,
          "explicit private-memory selection cannot silently override the fixed boot-memory profile");

    auto bytes = allocation_fixture();
    put<std::uint32_t>(bytes, 0x207, 2 * 1024 * 1024); // VirtualAlloc size immediate.
    // Original fixture returns BOOL(address != NULL), never a host address.
    const unsigned char return_success[] = {0x48, 0x85, 0xc0, 0x0f, 0x95, 0xc0, 0x0f,
                                            0xb6, 0xc0, 0x48, 0x83, 0xc4, 0x28, 0xc3};
    std::memcpy(bytes.data() + 0x21d, return_success, sizeof(return_success));
    const auto root = std::filesystem::temp_directory_path() / "ambrosia-private-memory-fixture";
    std::filesystem::create_directories(root);
    const auto path = root / "budget.exe";
    write_fixture(path, bytes);
    const std::vector<std::filesystem::path> small{"--trace-startup", path, "--private-memory-mib", "1"};
    output.str("");
    errors.str("");
    check(ambrosia::run_native_command(small, output, errors) == 3 &&
              output.str().find("\"trace_returned\":0") != std::string::npos,
          "selected private cap reaches the actual PE import and refuses a larger tiny allocation");
    const std::vector<std::filesystem::path> enough{"--trace-startup", path, "--private-memory-mib", "2"};
    output.str("");
    errors.str("");
    check(ambrosia::run_native_command(enough, output, errors) == 3 &&
              output.str().find("\"trace_returned\":1") != std::string::npos,
          "exact selected private budget permits the same PE allocation without weakening the test sandbox");
    std::filesystem::remove_all(root);
}
