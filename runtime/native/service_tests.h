// Original CPU fixtures for the supported service subset, not game inputs.
void memory_service_checks() {
    using namespace ambrosia::services;
    using ambrosia::host::Access;
    using ambrosia::host::Mapping;
    const auto page = Mapping::page_size();
    Mapping mapping(page * 2, Access::none, 65536);
    check(reinterpret_cast<std::uintptr_t>(mapping.data()) % 65536 == 0,
          "guest reservation alignment is 64 KiB");
    mapping.commit(0, page, Access::read_write);
    check(mapping.data()[0] == std::byte{0}, "fresh host commitment is zero-filled");
    mapping.data()[0] = std::byte{0x55};
    mapping.decommit(0, page);
    mapping.commit(0, page, Access::read_write);
    check(mapping.data()[0] == std::byte{0}, "host decommit discards contents before recommit");
    check(runtime_rejected([&] { mapping.decommit(page, page * 2); }),
          "host decommit cannot replace pages outside owned mapping");
    check(runtime_rejected([&] { mapping.commit(1, page, Access::read_write); }),
          "host commit rejects unaligned ranges");
    check(runtime_rejected([&] { Mapping invalid(page, Access::none, page + 1); }),
          "host mapping rejects invalid alignment");

    MemoryServices memory(page * 3);
    check(memory.allocate(nullptr, 0, 0x3000, 4).error == error_invalid_parameter,
          "zero-size guest allocation fails");
    check(memory.allocate(nullptr, 1, 0x103000, 4).error == error_not_supported,
          "unsupported allocation flags fail explicitly");
    check(memory.allocate(nullptr, 1, 0x3000, 0x40).error == error_not_supported,
          "writable-executable guest allocation is unsupported");
    check(memory.allocate(nullptr, SIZE_MAX, 0x3000, 4).error == error_invalid_parameter,
          "guest allocation rounding overflow fails");
    auto allocation = memory.allocate(nullptr, page + 1, 0x2000, 4);
    check(allocation.address && !allocation.error && memory.reserved_bytes() == page * 2,
          "reserve rounds size without consuming commitment");
    if (!allocation.address) {
        throw std::runtime_error("required synthetic allocation failed");
    }
    auto *base = static_cast<std::byte *>(allocation.address);
    check(memory.allocate(base, page, 0x3000, 4).error == error_not_supported,
          "fixed-address reservation does not overwrite host mappings");
    check(memory.allocate(base + page * 2, 1, 0x1000, 4).error == error_invalid_address,
          "commit rejects range beyond reservation");
    check(memory.allocate(base + page, SIZE_MAX, 0x1000, 4).error == error_invalid_address,
          "commit rejects overflowing end address");
    auto committed = memory.allocate(base + 1, page, 0x1000, 4);
    check(committed.address == base && !committed.error && base[0] == std::byte{0} &&
              base[page] == std::byte{0},
          "unaligned commit covers both intersecting owned pages");
    base[0] = std::byte{0x44};
    base[page] = std::byte{0x66};
    check(memory.allocate(base, page, 0x1000, 4).address == base && base[0] == std::byte{0x44},
          "recommit preserves existing contents");
    check(memory.allocate(base, page, 0x1000, 2).error == error_not_supported && base[0] == std::byte{0x44},
          "recommit with changed protection fails without changing memory");
    check(memory.free(base + page, 0, 0x8000).error == error_invalid_address,
          "release requires exact reservation base");
    check(memory.free(base, page, 0x8000).error == error_invalid_parameter, "release requires zero size");
    check(memory.free(base, 0, 0xc000).error == error_not_supported,
          "combined release flags are unsupported");
    check(memory.free(base + 1, 0, 0x4000).error == error_invalid_parameter,
          "whole decommit requires reservation base");
    check(memory.free(base + page, page + 1, 0x4000).error == error_invalid_address,
          "decommit cannot cross reservation boundary");
    check(memory.free(base + 1, page - 1, 0x4000).success && memory.reserved_bytes() == page * 2,
          "decommit preserves reservation and rounds to page boundaries");
    check(memory.allocate(base, page, 0x1000, 4).address == base && base[0] == std::byte{0} &&
              base[page] == std::byte{0x66},
          "guest recommit zeroes decommitted page without changing adjacent page");
    check(memory.free(base, 0, 0x4000).success && memory.free(base, 0, 0x4000).success,
          "whole reservation decommit is idempotent");
    check(memory.allocate(base, page * 2, 0x1000, 4).address == base && base[page] == std::byte{0},
          "whole decommit discards all committed pages");
    check(memory.allocate(nullptr, page + 1, 0x3000, 4).error == error_not_enough_memory,
          "allocation budget counts retained reservations");
    check(memory.free(base, 0, 0x8000).success && memory.reserved_bytes() == 0,
          "release restores reservation budget");
    check(memory.free(base, 0, 0x8000).error == error_invalid_address,
          "released address cannot be freed again");
    auto next = memory.allocate(nullptr, page, 0x1000, 4);
    check(next.address && memory.reserved_bytes() == page,
          "null-address commit creates a bounded reservation");
    check(memory.free(next.address, 0, 0x8000).success, "new reservation releases normally");
    check(memory.free(nullptr, 0, 0x8000).error == error_invalid_parameter, "null release fails explicitly");
    check(memory.allocate(mapping.data(), page, 0x1000, 4).error == error_invalid_address &&
              memory.free(mapping.data(), page, 0x4000).error == error_invalid_address,
          "guest services refuse unrelated host-owned mappings");
}

ambrosia::pe::Import service_symbol(const char *name, const char *module = "kernel32.dll") {
    ambrosia::pe::Import symbol;
    symbol.module = module;
    symbol.name = name;
    return symbol;
}

Bytes allocation_fixture() {
    auto bytes = fixture();
    text(bytes, 0x440, "kernel32.dll");
    text(bytes, 0x4a2, "VirtualAlloc");
    // shadow space + aligned stack; VirtualAlloc(NULL, 4096, RESERVE|COMMIT, RW).
    // Return the allocated address to the host test for validation and release.
    const unsigned char entry[] = {0x48, 0x83, 0xec, 0x28, 0x31, 0xc9, 0xba, 0,    0x10, 0,   0, 0x41,
                                   0xb8, 0,    0x30, 0,    0,    0x41, 0xb9, 4,    0,    0,   0, 0xff,
                                   0x15, 0,    0,    0,    0,    0x48, 0x83, 0xc4, 0x28, 0xc3};
    std::memcpy(bytes.data() + 0x200, entry, sizeof(entry));
    put<std::uint32_t>(bytes, 0x219, 0x2080 - (0x1000 + 29));
    return bytes;
}

Bytes error_fixture() {
    auto bytes = tls_fixture();
    text(bytes, 0x440, "kernelbase.dll");
    text(bytes, 0x4a2, "SetLastError");
    put<std::uint64_t>(bytes, 0x580, 0); // No TLS callbacks for this fixture.
    put<std::uint16_t>(bytes, 0x612, 0); // Remove callback-address relocation.
    // SetLastError(123), then read the same cell directly through GS:[0x68].
    const unsigned char entry[] = {0x48, 0x83, 0xec, 0x28, 0xb9, 123,  0,    0,    0,    0xff,
                                   0x15, 0,    0,    0,    0,    0x48, 0x83, 0xc4, 0x28, 0x65,
                                   0x8b, 0x04, 0x25, 0x68, 0,    0,    0,    0xc3};
    std::memcpy(bytes.data() + 0x200, entry, sizeof(entry));
    put<std::uint32_t>(bytes, 0x20b, 0x2080 - (0x1000 + 15));
    return bytes;
}

void service_dispatch_checks() {
    using namespace ambrosia::services;
    ImportRegistry registry;
    registry.add("KERNEL32.dll", "probe", 1);
    check(registry.resolve(service_symbol("probe")) == 1, "registry folds module case");
    check(!registry.resolve(service_symbol("Probe")), "registry preserves symbol case");
    check(!registry.resolve(service_symbol("unknown")), "unknown service remains unresolved");
    auto ordinal = service_symbol("probe");
    ordinal.by_ordinal = true;
    ordinal.ordinal = 1;
    check(!registry.resolve(ordinal), "registry does not guess ordinal services");
    check(runtime_rejected([&] { registry.add("kernel32.dll", "probe", 2); }),
          "duplicate service registration fails");
    check(runtime_rejected([&] { registry.add("../host.dll", "probe", 2); }),
          "registry rejects module traversal");
    check(runtime_rejected([&] { registry.add("kernel32.dll", "null", 0); }),
          "registry rejects null service addresses");

    using Allocate = void *(AMBROSIA_GUEST_ABI *)(void *, std::size_t, std::uint32_t, std::uint32_t);
    using Free = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, std::size_t, std::uint32_t);
    using GetError = std::uint32_t(AMBROSIA_GUEST_ABI *)();
    using SetError = void(AMBROSIA_GUEST_ABI *)(std::uint32_t);
    Runtime runtime;
    const auto allocate = reinterpret_cast<Allocate>(runtime.resolve(service_symbol("VirtualAlloc")));
    const auto free = reinterpret_cast<Free>(runtime.resolve(service_symbol("VirtualFree")));
    const auto get_error = reinterpret_cast<GetError>(runtime.resolve(service_symbol("GetLastError")));
    const auto set_error = reinterpret_cast<SetError>(runtime.resolve(service_symbol("SetLastError")));
    check(runtime.resolve(service_symbol("VirtualAlloc", "api-ms-win-core-memory-l1-1-6.dll")) ==
              reinterpret_cast<std::uintptr_t>(allocate),
          "inherited memory API alias resolves to the explicit native implementation");
    check(runtime.resolve(service_symbol("GetLastError", "api-ms-win-core-errorhandling-l1-1-0.dll")) ==
              reinterpret_cast<std::uintptr_t>(get_error),
          "error-handling API alias resolves explicitly");
    check(!runtime.resolve(service_symbol("VirtualProtect")), "unimplemented services are not stubs");
    check(!allocate(nullptr, 1, 0x3000, 4) && get_error() == error_invalid_function,
          "memory service requires an active runtime");
    {
        Scope scope(runtime);
        set_error(99);
        check(!allocate(nullptr, 0, 0x3000, 4) && get_error() == error_invalid_parameter,
              "guest ABI allocation failure sets last error");
        set_error(99);
        auto *address = allocate(nullptr, 4096, 0x3000, 4);
        check(address && get_error() == 99, "successful allocation preserves last error");
        {
            Runtime other;
            Scope nested(other);
            check(!free(address, 0, 0x8000) && get_error() == error_invalid_address,
                  "nested runtime cannot free another runtime's reservation");
        }
        set_error(98);
        check(free(address, 0, 0x8000) == 1 && get_error() == 98,
              "nested scope restores original runtime and BOOL release result");
        auto image = ambrosia::pe::Image::load(allocation_fixture(),
                                               [&](const auto &symbol) { return runtime.resolve(symbol); });
        const auto result = image.run_bootstrap();
        auto *guest_memory = reinterpret_cast<std::byte *>(result);
        check(guest_memory && runtime.memory().reserved_bytes() == 4096 && guest_memory[0] == std::byte{0},
              "synthetic PE imports and calls native VirtualAlloc with all four ABI arguments");
        check(free(guest_memory, 0, 0x8000) == 1, "PE allocation is owned and releasable by native runtime");
    }
    check(!allocate(nullptr, 1, 0x3000, 4) && get_error() == error_invalid_function,
          "leaving scope removes active runtime");
#if defined(__linux__) && defined(__x86_64__)
    set_error(77);
    ambrosia::guest::ThreadContext first, second;
    const auto old_gs = ambrosia::host::guest_segment_base();
    static thread_local std::uint32_t host_tls = 17;
    {
        auto active = first.activate();
        check(get_error() == 0, "new guest context has independent zero last error");
        set_error(11);
        {
            auto nested = second.activate();
            check(get_error() == 0, "nested guest context has separate last error");
            set_error(22);
            host_tls = 18;
        }
        check(get_error() == 11 && first.last_error() == 11 && second.last_error() == 22,
              "leaving nested guest context restores prior last error");
    }
    check(get_error() == 77 && host_tls == 18 && ambrosia::host::guest_segment_base() == old_gs,
          "guest error context preserves native FS TLS and restores host GS");
    ambrosia::pe::ModuleGraph graph([&](const auto &symbol) { return runtime.resolve(symbol); });
    graph.add("errors.exe", error_fixture());
    graph.load("errors.exe");
    {
        Scope scope(runtime);
        auto execution = graph.start();
        check(execution.run_bootstrap() == 123, "SetLastError import and direct GS error cell agree");
    }
    check(get_error() == 77, "guest entry last error does not leak into host fallback cell");

    const auto root = std::filesystem::temp_directory_path() / "ambrosia-service-cli";
    std::filesystem::create_directories(root);
    const auto path = root / "errors.exe";
    write_fixture(path, error_fixture());
    std::ostringstream output, errors;
    const std::vector<std::filesystem::path> arguments{"--run-bootstrap", path};
    check(ambrosia::run_native_command(arguments, output, errors) == 0 && errors.str().empty() &&
              output.str().find("\"bootstrap_result\":123") != std::string::npos &&
              output.str().find("\"files_read\":1") != std::string::npos,
          "CLI binds native services without a package copy of kernelbase.dll");
    auto allocation = allocation_fixture();
    // Return BOOL(address != NULL), keeping pointer values out of CLI output.
    const unsigned char return_success[] = {0x48, 0x85, 0xc0, 0x0f, 0x95, 0xc0, 0x0f,
                                            0xb6, 0xc0, 0x48, 0x83, 0xc4, 0x28, 0xc3};
    std::memcpy(allocation.data() + 0x21d, return_success, sizeof(return_success));
    write_fixture(path, allocation);
    output.str("");
    errors.str("");
    check(ambrosia::run_native_command(arguments, output, errors) == 0 && errors.str().empty() &&
              output.str().find("\"bootstrap_result\":1") != std::string::npos,
          "CLI keeps native memory services active during guest entry");
    std::filesystem::remove_all(root);
#endif
}
