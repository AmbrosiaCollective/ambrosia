void memory_budget_checks() {
    using namespace ambrosia::services;
    constexpr std::size_t page = 65536;
    MemoryServices separate(MemoryLimits{page, page * 2, page * 3});
    const auto window = separate.reserve_window(page * 2, 0x21, 4);
    const auto ordinary = separate.allocate(nullptr, page, 0x3000, 4);
    check(window.address && ordinary.address && separate.reserved_bytes() == page * 3,
          "sparse-window allowance is separate from the bounded ordinary allocation allowance");
    check(separate.allocate(nullptr, 1, 0x3000, 4).error == error_not_enough_memory &&
              separate.reserve_window(1, 3, 0x801).error == error_not_enough_memory,
          "each memory category retains its own cap even when sparse reservations are enabled");
    separate.free(window.address, 0, 0x8000);
    check(separate.allocate(nullptr, 1, 0x3000, 4).error == error_not_enough_memory &&
              separate.reserve_window(page * 2, 3, 0x801).address,
          "releasing a window reclaims its category but cannot inflate private allocation capacity");
    MemoryServices aggregate(MemoryLimits{page * 2, page * 2, page * 3});
    const auto private_region = aggregate.allocate(nullptr, page * 2, 0x3000, 4);
    check(private_region.address &&
              aggregate.reserve_window(page * 2, 3, 0x801).error == error_not_enough_memory &&
              aggregate.reserve_window(page, 3, 0x801).address && aggregate.reserved_bytes() == page * 3,
          "aggregate reservation cap constrains the sum of private and window categories");
    MemoryServices disabled(MemoryLimits{0, page, page});
    check(disabled.allocate(nullptr, 1, 0x3000, 4).error == error_not_enough_memory &&
              disabled.reserve_window(1, 3, 0x801).address,
          "disabled private allocation cannot borrow the available sparse-window budget");
    MemoryServices legacy(page);
    const auto legacy_window = legacy.reserve_window(1, 3, 0x801);
    check(legacy_window.address && legacy.allocate(nullptr, 1, 0x3000, 4).error == error_not_enough_memory,
          "single-budget construction preserves the original shared aggregate memory cap");
    Runtime runtime(MemoryLimits{page, page * 2, page * 3});
    check(runtime.memory().reserve_window(page * 2, 0x21, 4).address &&
              runtime.memory().allocate(nullptr, page, 0x3000, 4).address,
          "runtime forwards explicit typed memory limits to the shared memory owner");
    std::ostringstream output, errors;
    const std::vector<std::filesystem::path> inspect{"--inspect", "unused", "--sparse-windows"};
    check(ambrosia::run_native_command(inspect, output, errors) == 2 &&
              errors.str().find("requires trace-startup") != std::string::npos,
          "sparse-window allowance is an explicit diagnostic option and cannot alter inspection mode");
    output.str("");
    errors.str("");
    const std::vector<std::filesystem::path> duplicate{"--trace-startup", "unused", "--sparse-windows",
                                                       "--sparse-windows"};
    check(ambrosia::run_native_command(duplicate, output, errors) == 2 &&
              errors.str().find("duplicate sparse-window") != std::string::npos,
          "duplicate sparse-window options are rejected before any input is loaded");
}
