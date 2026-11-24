std::vector<unsigned> crt_events;
ambrosia::services::OnExitTables *callback_tables = nullptr;
void *callback_table = nullptr;

std::int32_t AMBROSIA_GUEST_ABI crt_first() {
    crt_events.push_back(1);
    return 0;
}

std::int32_t AMBROSIA_GUEST_ABI crt_second() {
    crt_events.push_back(2);
    return 7;
}

std::int32_t AMBROSIA_GUEST_ABI crt_register_later() {
    crt_events.push_back(3);
    if (callback_tables->add(callback_table, &crt_first)) {
        throw std::runtime_error("synthetic reentrant on-exit registration failed");
    }
    return 0;
}

void AMBROSIA_GUEST_ABI crt_void_initializer() {
    crt_events.push_back(4);
}

void crt_lifecycle_checks() {
    using namespace ambrosia::services;
    OnExitTables tables;

    struct Guarded {
        std::uint64_t before = 0x1234;
        OnExitTable table{UINTPTR_MAX, UINTPTR_MAX, UINTPTR_MAX};
        std::uint64_t after = 0x5678;
    } object;

    check(tables.initialize(nullptr) == -1, "null on-exit table fails explicitly");
    check(tables.initialize(&object.table) == 0 && object.table.first == 0 && object.table.last == 0 &&
              object.table.end == 0,
          "on-exit initialization normalizes an empty encoded/sentinel table");
    check(object.before == 0x1234 && object.after == 0x5678,
          "on-exit initialization touches exactly 24 bytes");
    check(tables.add(&object.table, &crt_first) == 0 && tables.add(&object.table, nullptr) == 0 &&
              tables.add(&object.table, &crt_second) == 0,
          "on-exit table owns callbacks including nullable entries");
    check(object.table.first && object.table.last - object.table.first == 24 &&
              object.table.end >= object.table.last,
          "on-exit guest view reflects owned pointer array extent");
    const auto before = object.table.last;
    check(tables.initialize(&object.table) == 0 && object.table.last == before,
          "repeat initialization does not drop pending callbacks");
    crt_events.clear();
    check(tables.execute(&object.table) == 0 && crt_events == std::vector<unsigned>{2, 1},
          "on-exit callbacks execute in reverse order and ignore callback return values");
    check(!object.table.first && !object.table.last && !object.table.end,
          "executed on-exit table is cleared before reuse");
    check(tables.execute(&object.table) == 0 && crt_events.size() == 2,
          "executing a cleared table is idempotent");
    callback_tables = &tables;
    callback_table = &object.table;
    check(tables.add(&object.table, &crt_register_later) == 0,
          "on-exit table accepts reentrant-registration fixture");
    crt_events.clear();
    check(tables.execute(&object.table) == 0 && crt_events == std::vector<unsigned>{3} && object.table.first,
          "callback-time registrations form a later batch without lock reentrancy");
    check(tables.execute(&object.table) == 0 && crt_events == std::vector<unsigned>{3, 1},
          "later execution runs callbacks registered by a previous callback");
    object.table.first = 1;
    check(tables.add(&object.table, &crt_first) == -1,
          "foreign/corrupted on-exit pointers are never followed");
    object.table = {};
    OnExitTable bounded;
    bool filled = true;
    for (unsigned index = 0; index < 4096; ++index) {
        filled = filled && tables.add(&bounded, nullptr) == 0;
    }
    check(filled && tables.add(&bounded, nullptr) == -1, "per-table on-exit callback budget is enforced");
    check(tables.execute(&bounded) == 0, "bounded null callback batch clears without invocation");

    Runtime runtime;
    Scope scope(runtime);
    using Initialize = std::int32_t(AMBROSIA_GUEST_ABI *)(void *);
    using Register = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, OnExitCallback);
    using InitVoid = void(AMBROSIA_GUEST_ABI *)(const void *, const void *);
    using InitError = std::int32_t(AMBROSIA_GUEST_ABI *)(const void *, const void *);
    const auto initialize = reinterpret_cast<Initialize>(
        runtime.resolve(service_symbol("_initialize_onexit_table", "api-ms-win-crt-runtime-l1-1-0.dll")));
    const auto add = reinterpret_cast<Register>(
        runtime.resolve(service_symbol("_register_onexit_function", "ucrtbase.dll")));
    const auto execute = reinterpret_cast<Initialize>(
        runtime.resolve(service_symbol("_execute_onexit_table", "ucrtbase.dll")));
    const auto init_void =
        reinterpret_cast<InitVoid>(runtime.resolve(service_symbol("_initterm", "ucrtbase.dll")));
    const auto init_error =
        reinterpret_cast<InitError>(runtime.resolve(service_symbol("_initterm_e", "ucrtbase.dll")));
    OnExitTable abi_table;
    crt_events.clear();
    check(initialize(&abi_table) == 0 && add(&abi_table, &crt_first) == 0 && execute(&abi_table) == 0 &&
              crt_events == std::vector<unsigned>{1},
          "CRT table wrappers preserve Microsoft x64 argument and callback ABI");
    const std::array<OnExitCallback, 4> checked{&crt_first, nullptr, &crt_second, &crt_first};
    crt_events.clear();
    check(init_error(checked.data(), checked.data() + checked.size()) == 7 &&
              crt_events == std::vector<unsigned>{1, 2},
          "error initializer range stops at the first nonzero result");
    const std::array<Initializer, 3> initializers{nullptr, &crt_void_initializer, &crt_void_initializer};
    crt_events.clear();
    init_void(initializers.data(), initializers.data() + initializers.size());
    check(crt_events == std::vector<unsigned>{4, 4},
          "void initializer range skips nulls and preserves forward order");
    check(init_error(nullptr, nullptr) == 0, "empty initializer range has no callbacks or error");
    init_void(nullptr, nullptr);
}
