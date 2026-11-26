struct OpenSelfRecord {
    bool opened = false;
    bool metadata = false;
    std::uint32_t owner_id = 0;
    bool owner_opened = false;
};

std::uint32_t AMBROSIA_GUEST_ABI reopen_current_thread(void *argument) {
    using namespace ambrosia::services;
    auto &record = *static_cast<OpenSelfRecord *>(argument);
    auto *runtime = current_runtime();
    using Open = std::uintptr_t(AMBROSIA_GUEST_ABI *)(std::uint32_t, std::int32_t, std::uint32_t);
    const auto open = reinterpret_cast<Open>(runtime->resolve(service_symbol("OpenThread", "kernel32.dll")));
    const auto id = ambrosia::host::current_thread_id();
    const auto handle = open(thread_all_access, 0, id);
    record.opened = handle != 0;
    record.metadata = runtime->threads().thread_id(handle) == id &&
                      runtime->objects().wait(handle, 0).result == wait_timeout;
    (void)runtime->objects().close(handle);
    const auto owner = open(thread_all_access, 0, record.owner_id);
    record.owner_opened = owner && runtime->threads().thread_id(owner) == record.owner_id &&
                          runtime->objects().wait(owner, 0).result == wait_timeout;
    (void)runtime->objects().close(owner);
    return 43;
}

void open_thread_checks() {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    pe::ModuleGraph graph([&](const auto &symbol) { return runtime.resolve(symbol); });
    auto bytes = native_thread_fixture();
    jump_to_host(bytes, 0x380, reinterpret_cast<std::uintptr_t>(&reopen_current_thread));
    graph.add("open.exe", bytes);
    graph.load("open.exe");
    Scope scope(runtime, &graph);
    auto process = std::unique_ptr<pe::ModuleGraph::Execution>(new pe::ModuleGraph::Execution(graph.start()));
    using Create =
        std::uintptr_t(AMBROSIA_GUEST_ABI *)(void *, std::size_t, ThreadEntry, void *, std::uint32_t, void *);
    using Open = std::uintptr_t(AMBROSIA_GUEST_ABI *)(std::uint32_t, std::int32_t, std::uint32_t);
    using Identify = std::uint32_t(AMBROSIA_GUEST_ABI *)(std::uintptr_t);
    using Resume = std::uint32_t(AMBROSIA_GUEST_ABI *)(std::uintptr_t);
    const auto create =
        reinterpret_cast<Create>(runtime.resolve(service_symbol("CreateThread", "kernel32.dll")));
    const auto open = reinterpret_cast<Open>(
        runtime.resolve(service_symbol("OpenThread", "api-ms-win-core-processthreads-l1-1-0.dll")));
    const auto identify =
        reinterpret_cast<Identify>(runtime.resolve(service_symbol("GetThreadId", "kernel32.dll")));
    const auto resume =
        reinterpret_cast<Resume>(runtime.resolve(service_symbol("ResumeThread", "kernel32.dll")));
    const auto fast = reinterpret_cast<ThreadEntry>(graph.loaded_module() + 0x1180);
    OpenSelfRecord record;
    record.owner_id = host::current_thread_id();
    std::uintptr_t owner_handle = 0;
    bool cleanup_pending = false;
    process->with_guest_context([&] {
        owner_handle = open(thread_all_access, 0, record.owner_id);
        guest::ThreadContext::current()->on_exit([&] {
            std::uint32_t cleanup_code = 0;
            const auto error = runtime.threads().exit_code(owner_handle, &cleanup_code);
            cleanup_pending = !error && cleanup_code == 259 &&
                              runtime.objects().wait(owner_handle, 0).result == wait_timeout;
        });
    });
    check(owner_handle && identify(owner_handle) == record.owner_id &&
              runtime.objects().wait(owner_handle, 0).result == wait_timeout,
          "the original guest context registers its real owner identity and retains unsignaled lifetime");
    std::uint32_t id = 0;
    const auto original = create(nullptr, 65536, fast, &record, 4, &id);
    const auto saved_error = errno;
    errno = EDOM;
    crt_error() = 61;
    set_last_error(91);
    const auto reopened = open(thread_all_access, 1, id);
    check(original && reopened && original != reopened && identify(reopened) == id &&
              runtime.objects().reference(reopened).inherited && errno == EDOM && crt_error() == 61 &&
              get_last_error() == 91,
          "OpenThread creates a distinct inherited-marked handle to the same live guest job and preserves "
          "successful error state");
    const auto closed_original = runtime.objects().close(original);
    check(!closed_original && runtime.objects().wait(reopened, 0).result == wait_timeout &&
              identify(reopened) == id,
          "closing the creation handle leaves the reopened handle and worker lifetime intact");
    const auto query_only = open(0x40, 0, id);
    const auto sync_only = open(object_synchronize, 0, id);
    const auto no_access = open(0, 0, id);
    const auto limited_query = open(0x800, 0, id);
    std::uint32_t code = 0;
    const auto query_error = runtime.threads().exit_code(query_only, &code);
    const auto denied_wait = runtime.objects().wait(query_only, 0);
    const auto sync_query = identify(sync_only);
    const auto sync_query_error = get_last_error();
    check(query_only && sync_only && no_access && limited_query && !query_error && code == 259 &&
              identify(query_only) == id && identify(limited_query) == id &&
              denied_wait.result == wait_failed && denied_wait.error == 5 && !sync_query &&
              sync_query_error == 5 && runtime.objects().wait(sync_only, 0).result == wait_timeout,
          "query, limited-query and synchronize handles enforce independent per-handle access to the shared "
          "thread state");
    const auto denied_resume = resume(query_only);
    const auto resume_error = get_last_error();
    const auto denied_priority = runtime.threads().set_priority(query_only, 0);
    const auto denied_identity = identify(no_access);
    check(
        denied_resume == 0xffffffffu && resume_error == 5 && denied_priority == 5 && !denied_identity &&
            get_last_error() == 5,
        "restricted reopened handles cannot resume, schedule or query a thread without the required rights");
    const ThreadAffinity mapping(runtime.processor_information().processor_ids());
    GroupAffinity affinity;
    const auto affinity_read = runtime.threads().group_affinity(query_only, mapping, affinity);
    GroupAffinity previous;
    const auto affinity_write = runtime.threads().set_group_affinity(query_only, mapping, affinity, previous);
    check(!affinity_read && affinity.mask && affinity_write == 5,
          "query-only handles can observe real CPU affinity but cannot change it through another API");
    const auto only_resume = open(thread_resume_access, 0, id);
    const auto resumed = resume(only_resume);
    const auto wait_result = runtime.objects().wait(reopened, 2000);
    const auto exit_error = runtime.threads().exit_code(reopened, &code);
    check(only_resume && resumed == 1 && wait_result.result == wait_object_0 && !exit_error && code == 43 &&
              record.opened && record.metadata && record.owner_opened,
          "a resume-only handle starts original PE entry; that worker reopens its own real ID and shared "
          "completion retains its actual result");
    const auto ended_open = open(thread_all_access, 0, id);
    const auto ended_error = get_last_error();
    const auto zero_id = open(thread_all_access, 0, 0);
    check(!ended_open && ended_error == 87 && !zero_id && get_last_error() == 87,
          "finished and zero thread IDs cannot be reopened as a foreign or recycled host thread");
    for (const auto handle : {reopened, query_only, sync_only, no_access, limited_query, only_resume}) {
        (void)runtime.objects().close(handle);
    }
    check(!identify(reopened) && get_last_error() == 6,
          "closing each reopened handle invalidates it without altering the retained thread result of other "
          "handles");
    runtime.threads().shutdown();
    runtime.threads().owner_returned(77);
    std::uint32_t owner_code = 0;
    const auto live_code_error = runtime.threads().exit_code(owner_handle, &owner_code);
    check(!live_code_error && owner_code == 259 &&
              runtime.objects().wait(owner_handle, 0).result == wait_timeout,
          "recording the owner entry result does not signal or expose it before guest cleanup");
    process->close();
    process.reset();
    const auto owner_exit_error = runtime.threads().exit_code(owner_handle, &owner_code);
    const auto reopened_owner = open(thread_all_access, 0, record.owner_id);
    check(cleanup_pending && !owner_exit_error && owner_code == 77 &&
              runtime.objects().wait(owner_handle, 0).result == wait_object_0 && !reopened_owner &&
              get_last_error() == 87,
          "owner completion follows final guest cleanup, reports the actual entry result and prevents "
          "reopening the ended guest lifetime");
    (void)runtime.objects().close(owner_handle);
    errno = saved_error;
}
