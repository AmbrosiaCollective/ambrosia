std::size_t title_allocations = 0;
std::size_t title_releases = 0;
std::size_t title_requested_bytes = 0;
std::uint64_t title_requested_attributes = 0;
bool title_callback_context = false;

void *AMBROSIA_GUEST_ABI synthetic_title_allocate(std::size_t bytes, std::uint64_t attributes) {
    using namespace ambrosia;
    auto *runtime = services::current_runtime();
    title_callback_context = runtime && services::current_modules() && guest::ThreadContext::current();
    ++title_allocations;
    title_requested_bytes = bytes;
    title_requested_attributes = attributes;
    errno = ERANGE;
    return runtime->memory().allocate(nullptr, bytes, 0x3000, 4).address;
}

void AMBROSIA_GUEST_ABI synthetic_title_free(void *address, std::uint64_t attributes) {
    using namespace ambrosia;
    check(attributes == services::descriptor_memory_attributes && services::current_runtime() &&
              guest::ThreadContext::current(),
          "title release callback retains its actual attributes and guest context");
    ++title_releases;
    check(services::current_runtime()->memory().free(address, 0, 0x8000).success,
          "title release callback frees its own selected allocation");
    errno = EBADF;
}

Bytes title_hook_fixture() {
    auto bytes = provider_fixture();
    jump_to_host(bytes, 0x200, reinterpret_cast<std::uintptr_t>(&synthetic_title_allocate));
    jump_to_host(bytes, 0x220, reinterpret_cast<std::uintptr_t>(&synthetic_title_free));
    put<std::uint64_t>(bytes, 0x500, 0x180001000ull);
    put<std::uint64_t>(bytes, 0x508, 0x180001020ull);
    put<std::uint16_t>(bytes, 0x608, 0xa100);
    put<std::uint16_t>(bytes, 0x60a, 0xa108);
    return exported_fixture(std::move(bytes), {{"__XMemAllocationHooks__", {}, 0x2100}});
}

void title_memory_checks() {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    pe::ModuleGraph graph;
    graph.add("hooks.dll", title_hook_fixture());
    graph.load("hooks.dll");
    const auto module = graph.loaded_module();
    const auto table = graph.loaded_export(module, named_symbol("__XMemAllocationHooks__"));
    check(table.has_value(), "title hooks resolve as an owned loaded data export");
    if (!table) {
        return;
    }
    std::array<std::uintptr_t, 2> pointers{};
    check(graph.copy_loaded_data(*table, std::as_writable_bytes(std::span(pointers))) &&
              pointers[0] == module + 0x1000 && pointers[1] == module + 0x1020,
          "bounded loaded-data reads resolve both relocated hook pointers");
    std::array<std::byte, 65> large{};
    check(!graph.copy_loaded_data(*table, large) &&
              !graph.copy_loaded_data(module + 0x21f8, std::span(large).first(16)) &&
              !graph.copy_loaded_data(module, std::span(large).first(1)) &&
              !graph.copy_loaded_data(module + 0x1000, std::span(large).first(1)) &&
              !graph.copy_loaded_data(UINTPTR_MAX - 4, std::span(large).first(8)),
          "oversize, section-crossing, header, code and wrapping data-table reads fail before dereference");
    check(!graph.active_code_address(pointers[0]) &&
              runtime_rejected([&] { (void)graph.execution_generation(); }),
          "mapped code is not a callable title hook before an execution opens");
    check(runtime_rejected([&] { (void)allocate_title_descriptors(64); }),
          "title allocator calls require a scoped runtime, graph and guest context");
    Scope scope(runtime, &graph);
    auto execution = graph.start();
    check(graph.active_code_address(pointers[0]) && !graph.active_code_address(*table) &&
              !graph.active_code_address(reinterpret_cast<std::uintptr_t>(&synthetic_title_allocate)),
          "only active owned executable-section addresses qualify as guest callbacks");
    check(runtime_rejected([&] { (void)allocate_title_descriptors(64); }),
          "an open graph cannot substitute for active guest TLS on the calling thread");
    title_allocations = title_releases = 0;
    const auto saved_errno = errno;
    execution.with_guest_context([&] {
        errno = E2BIG;
        auto allocation = allocate_title_descriptors(97);
        check(title_callback_context && title_requested_bytes == 97 &&
                  title_requested_attributes == descriptor_memory_attributes && allocation.size() == 97 &&
                  allocation.data() && errno == E2BIG,
              "the loaded hook receives the exact descriptor ABI and returns title-selected storage");
        auto moved = std::move(allocation);
        check(!allocation.data() && moved.data() && moved.size() == 97,
              "moving a title allocation transfers explicit release ownership");
        moved.close();
        moved.close();
        check(!moved.data() && title_allocations == 1 && title_releases == 1 && errno == E2BIG,
              "explicit title release is idempotent and preserves host errno");
        check(runtime_rejected([&] { (void)allocate_title_descriptors(0); }) &&
                  runtime_rejected([&] { (void)allocate_title_descriptors(64 * 1024 * 1024 + 1); }),
              "invalid descriptor allocation sizes cannot reach guest code");
        const auto saved = pointers;
        auto *mutable_table = reinterpret_cast<std::uintptr_t *>(*table);
        mutable_table[0] = *table;
        check(runtime_rejected([&] { (void)allocate_title_descriptors(64); }) && title_allocations == 1,
              "a changed hook table is reread and data pointers cannot become callable code");
        mutable_table[0] = saved[0];
        mutable_table[1] = 0;
        check(runtime_rejected([&] { (void)allocate_title_descriptors(64); }) && title_allocations == 1,
              "a missing paired free callback rejects allocation before invoking the allocator");
        mutable_table[1] = saved[1];
        auto retained = allocate_title_descriptors(64);
        {
            Runtime foreign;
            Scope foreign_scope(foreign, &graph);
            check(runtime_rejected([&] { retained.close(); }) && retained.data(),
                  "foreign runtime scope cannot free a title allocation or consume its ownership");
        }
        retained.close();
    });
    errno = saved_errno;
    std::optional<TitleMemoryAllocation> stale;
    execution.with_guest_context([&] { stale.emplace(allocate_title_descriptors(64)); });
    auto *stale_address = stale->data();
    execution.close();
    check(!graph.active_code_address(pointers[0]) && runtime_rejected([&] { stale->close(); }),
          "closed execution cannot invoke a retained title free callback");
    {
        auto restarted = graph.start();
        restarted.with_guest_context([&] {
            const auto released_before = title_releases;
            check(runtime_rejected([&] { stale->close(); }) && stale->data() == stale_address &&
                      title_releases == released_before,
                  "a new module generation cannot consume old title allocation ownership");
        });
        restarted.close();
    }
    // The synthetic allocator uses MemoryServices; retire fixture storage from
    // the host after proving no stale guest callback was invoked.
    check(runtime.memory().free(stale_address, 0, 0x8000).success && runtime.memory().reserved_bytes() == 0,
          "synthetic stale-generation storage can be cleaned without calling guest code");
    {
        pe::ModuleGraph ambiguous;
        auto first =
            exported_fixture(title_hook_fixture(), {{"__XMemAllocationHooks__", {}, 0x2100},
                                                    {"dependency", "second.__XMemAllocationHooks__", 0}});
        ambiguous.add("first.dll", first);
        ambiguous.add("second.dll", title_hook_fixture());
        ambiguous.load("first.dll");
        (void)ambiguous.export_address("first.dll", named_symbol("dependency"));
        check(ambiguous.initialization_order().size() == 2,
              "the ambiguity fixture initializes both distinct hook providers");
        Scope nested(runtime, &ambiguous);
        auto process = ambiguous.start();
        process.with_guest_context([&] {
            const auto allocated_before = title_allocations;
            check(runtime_rejected([&] { (void)allocate_title_descriptors(64); }) &&
                      title_allocations == allocated_before,
                  "distinct loaded hook tables are rejected as ambiguous before any callback");
        });
    }
}
