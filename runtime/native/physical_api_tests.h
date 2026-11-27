std::uint64_t AMBROSIA_GUEST_ABI physical_command_entry() {
    using namespace ambrosia::services;
    auto *runtime = current_runtime();
    if (!runtime) {
        return 99;
    }
    using Allocate = std::int32_t(AMBROSIA_GUEST_ABI *)(std::uint64_t, void *, void *);
    using Free = std::int32_t(AMBROSIA_GUEST_ABI *)(std::uint64_t, const void *);
    const auto allocate =
        reinterpret_cast<Allocate>(runtime->resolve(service_symbol("XMemAllocatePhysicalPages", "xmem.dll")));
    const auto release =
        reinterpret_cast<Free>(runtime->resolve(service_symbol("XMemFreePhysicalPages", "xmem.dll")));
    if (!allocate || !release) {
        return 98;
    }
    std::uint64_t count = 2;
    std::array<std::uint64_t, 2> pages{};
    if (!allocate(0x280, &count, pages.data()) || count != 2 || !pages[0] || !pages[1] ||
        pages[0] == pages[1] || !release(count, pages.data()) ||
        runtime->physical_pages().available() != runtime->physical_pages().capacity()) {
        return 97;
    }
    return 77;
}

void physical_api_checks(int descriptor) {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    Scope scope(runtime);
    guest::ThreadContext context;
    auto activation = context.activate();
    using Allocate = std::int32_t(AMBROSIA_GUEST_ABI *)(std::uint64_t, void *, void *);
    using Free = std::int32_t(AMBROSIA_GUEST_ABI *)(std::uint64_t, const void *);
    const auto allocation_address = runtime.resolve(service_symbol("XMemAllocatePhysicalPages", "xmem.dll"));
    const auto release_address = runtime.resolve(service_symbol("XMemFreePhysicalPages", "xmem.dll"));
    check(allocation_address && release_address,
          "physical page allocation/free imports resolve to typed native Windows ABI services");
    if (!allocation_address || !release_address) {
        return;
    }
    runtime.physical_pages().bind(
        host::SharedBacking::adopt_descriptor(descriptor, PhysicalPages::maximum_backing_bytes));
    const auto allocate = reinterpret_cast<Allocate>(allocation_address);
    const auto release = reinterpret_cast<Free>(release_address);
    const auto saved_error = errno;
    errno = E2BIG;
    crt_error() = 19;
    set_last_error(83);
    std::uint64_t empty = 0;
    check(allocate(0x280, &empty, nullptr) == 1 && release(0, nullptr) == 1 && empty == 0 && errno == E2BIG &&
              crt_error() == 19 && get_last_error() == 83,
          "empty physical requests own no pages and preserve all three error domains");
    std::uint64_t count = 2;
    check(allocate(0x280, nullptr, &count) == 0 && get_last_error() == error_invalid_parameter &&
              allocate(0x280, &count, nullptr) == 0 && count == 2 &&
              get_last_error() == error_invalid_parameter,
          "missing count or nonempty page-array outputs fail without inventing partial allocation");
    std::array<std::byte, 10> packed_count;
    std::array<std::byte, 18> packed_pages;
    packed_count.fill(std::byte{0xcc});
    packed_pages.fill(std::byte{0xcc});
    std::memcpy(packed_count.data() + 1, &count, sizeof(count));
    set_last_error(83);
    const auto allocated = allocate(0x280, packed_count.data() + 1, packed_pages.data() + 1);
    std::array<std::uint64_t, 2> tokens{};
    std::memcpy(&count, packed_count.data() + 1, sizeof(count));
    std::memcpy(tokens.data(), packed_pages.data() + 1, sizeof(tokens));
    check(
        allocated == 1 && count == 2 && tokens[0] && tokens[1] && tokens[0] != tokens[1] &&
            runtime.physical_pages().lease(tokens[0]) && runtime.physical_pages().lease(tokens[1]) &&
            packed_count.front() == std::byte{0xcc} && packed_count.back() == std::byte{0xcc} &&
            packed_pages.front() == std::byte{0xcc} && packed_pages.back() == std::byte{0xcc},
        "packed Windows count/array outputs receive exactly their owned page IDs without overwriting guards");
    if (!allocated) {
        errno = saved_error;
        return;
    }
    const auto original_pages = packed_pages;
    check(release(count, packed_pages.data() + 1) == 1 && packed_pages == original_pages &&
              runtime.physical_pages().available() == runtime.physical_pages().capacity() && errno == E2BIG &&
              crt_error() == 19 && get_last_error() == 83,
          "packed free input is unchanged and successful physical services preserve native and guest errors");
    count = runtime.physical_pages().capacity() + 1;
    std::memcpy(packed_count.data() + 1, &count, sizeof(count));
    const auto over_budget = allocate(0x280, packed_count.data() + 1, packed_pages.data() + 1);
    std::memcpy(&count, packed_count.data() + 1, sizeof(count));
    check(over_budget == 0 && count == 0 && get_last_error() == error_not_enough_memory &&
              packed_pages == original_pages && packed_count.front() == std::byte{0xcc} &&
              packed_count.back() == std::byte{0xcc},
          "budget failure clears only the in-out count and preserves page-array bytes and packed guards");
    count = UINT64_MAX;
    check(allocate(0x280, &count, packed_pages.data() + 1) == 0 && count == 0 &&
              get_last_error() == error_not_enough_memory && packed_pages == original_pages,
          "an overflowing64-bit request fails before multiplying or writing a page array");
    check(release(1, nullptr) == 0 && get_last_error() == error_invalid_parameter &&
              release(UINT64_MAX, packed_pages.data() + 1) == 0 &&
              get_last_error() == error_invalid_parameter,
          "invalid free pointers/counts cannot read an unbounded page array");
    count = 2;
    tokens.fill(0);
    const bool prefix_allocated = allocate(0x280, &count, tokens.data()) == 1;
    const std::array duplicate{tokens[0], tokens[0]};
    const auto prefix_result = prefix_allocated ? release(2, duplicate.data()) : 99;
    const bool prefix_valid = prefix_result == 0 && get_last_error() == error_invalid_parameter &&
                              !runtime.physical_pages().lease(tokens[0]) &&
                              runtime.physical_pages().lease(tokens[1]);
    const auto tail_result = prefix_allocated ? release(1, &tokens[1]) : 99;
    check(prefix_allocated && prefix_valid && tail_result == 1 &&
              runtime.physical_pages().available() == runtime.physical_pages().capacity(),
          "physical free preserves valid-prefix effects without a duplicate ID freeing another allocation");
    auto bytes = fixture();
    text(bytes, 0x440, "xmem.dll");
    text(bytes, 0x4a2, "XMemAllocatePhysicalPages");
    put<std::uint64_t>(bytes, 0x541, 2);
    bytes[0x540] = bytes[0x549] = bytes[0x550] = bytes[0x561] = std::byte{0xcc};
    constexpr unsigned char code[] = {0x48, 0x83, 0xec, 0x28, 0xb9, 0x80, 2,    0,    0,    0x48, 0x8d, 0x15,
                                      0,    0,    0,    0,    0x4c, 0x8d, 0x05, 0,    0,    0,    0,    0xff,
                                      0x15, 0,    0,    0,    0,    0x48, 0x83, 0xc4, 0x28, 0xc3};
    std::memcpy(bytes.data() + 0x200, code, sizeof(code));
    put<std::uint32_t>(bytes, 0x20c, 0x2141 - (0x1000 + 16));
    put<std::uint32_t>(bytes, 0x213, 0x2151 - (0x1000 + 23));
    put<std::uint32_t>(bytes, 0x219, 0x2080 - (0x1000 + 29));
    auto image = pe::Image::load(bytes, [&](const auto &symbol) { return runtime.resolve(symbol); });
    errno = E2BIG;
    crt_error() = 19;
    set_last_error(83);
    const auto returned = image.run_bootstrap();
    std::memcpy(&count, image.base() + 0x2141, sizeof(count));
    std::memcpy(tokens.data(), image.base() + 0x2151, sizeof(tokens));
    check(returned == 1 && count == 2 && tokens[0] && tokens[1] && image.base()[0x2140] == std::byte{0xcc} &&
              image.base()[0x2149] == std::byte{0xcc} && image.base()[0x2150] == std::byte{0xcc} &&
              image.base()[0x2161] == std::byte{0xcc},
          "an original PE supplies all three physical-allocation arguments through registers and packed "
          "guest outputs");
    check(release(count, tokens.data()) == 1 && errno == E2BIG && crt_error() == 19 && get_last_error() == 83,
          "the original PE allocation is owned by the same runtime and releases through its typed service");
    errno = saved_error;
}

void physical_command_checks(int descriptor) {
    using namespace ambrosia;
    auto bytes = fixture();
    put<std::uint32_t>(bytes, 0x98 + 120, 0);
    put<std::uint32_t>(bytes, 0x98 + 124, 0);
    jump_to_host(bytes, 0x200, reinterpret_cast<std::uintptr_t>(&physical_command_entry));
    const auto root = std::filesystem::path("/work/physical-command");
    std::filesystem::create_directories(root);
    const auto image = root / "original.exe";
    {
        std::ofstream file(image, std::ios::binary);
        file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    for (const auto &arguments :
         {std::vector<std::filesystem::path>{"--inspect", image, "--physical-backing-fd", "3"},
          std::vector<std::filesystem::path>{"--run-bootstrap", image, "--physical-backing-fd", "2"},
          std::vector<std::filesystem::path>{"--run-bootstrap", image, "--physical-backing-fd", "3",
                                             "--physical-backing-fd", "3"}}) {
        std::ostringstream output;
        std::ostringstream errors;
        check(run_native_command(arguments, output, errors) == 2 && !errors.str().empty(),
              "invalid physical descriptor/mode/duplicate arguments fail before descriptor transfer");
    }
    std::ostringstream output;
    std::ostringstream errors;
    check(
        run_native_command(std::vector<std::filesystem::path>{"--run-bootstrap", image, "--package", root,
                                                              "--physical-backing-fd",
                                                              std::to_string(descriptor)},
                           output, errors) == 0 &&
            errors.str().empty() && output.str().find("\"bootstrap_result\":77") != std::string::npos,
        "the actual native command binds owned backing before an original PE allocates and frees its pages");
}
