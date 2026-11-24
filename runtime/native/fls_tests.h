std::vector<void *> fls_callback_values;
bool fls_callback_context = true;

void AMBROSIA_GUEST_ABI record_fls_value(void *value) {
    fls_callback_context = fls_callback_context && ambrosia::guest::ThreadContext::current();
    fls_callback_values.push_back(value);
}

void fls_checks() {
    using namespace ambrosia;
    using namespace ambrosia::services;
    FlsSlots slots;
    auto allocation = slots.allocate(&record_fls_value);
    check(allocation.index != fls_out_of_indexes && !allocation.error,
          "FLS allocation returns an owned process-local index");
    const auto index = allocation.index;
    auto first = std::make_unique<guest::ThreadContext>();
    auto second = std::make_unique<guest::ThreadContext>();
    std::uint64_t first_value = 1, second_value = 2;
    check(!slots.get(index, *first).value && !slots.get(index, *first).error,
          "new context has a zero-initialized FLS value");
    check(slots.set(index, &first_value, *first) == 0 && slots.set(index, &second_value, *second) == 0,
          "two guest contexts can store independent values in the same index");
    check(slots.get(index, *first).value == &first_value && slots.get(index, *second).value == &second_value,
          "FLS values do not leak between guest contexts");
    check(slots.get(0, *first).error == error_invalid_parameter &&
              slots.set(fls_out_of_indexes, nullptr, *first) == error_invalid_parameter,
          "invalid FLS indices fail explicitly");
    fls_callback_values.clear();
    fls_callback_context = true;
    {
        auto active = first->activate();
        check(slots.free(index) == 0, "FLS free releases a process-wide index");
    }
    check(fls_callback_values.size() == 2 &&
              std::find(fls_callback_values.begin(), fls_callback_values.end(), &first_value) !=
                  fls_callback_values.end() &&
              std::find(fls_callback_values.begin(), fls_callback_values.end(), &second_value) !=
                  fls_callback_values.end(),
          "FLS free invokes the callback for every non-null context value");
    check(fls_callback_context, "FLS free callbacks execute in the calling guest context");
    check(slots.free(index) == error_invalid_parameter, "freed index cannot be freed twice");
    const auto reused = slots.allocate(&record_fls_value);
    check(reused.index == index && !slots.get(index, *first).value && !slots.get(index, *second).value,
          "reused FLS index starts with cleared values in existing contexts");
    check(slots.set(index, &first_value, *first) == 0, "reused index accepts a new value");
    fls_callback_values.clear();
    const auto original_gs = host::guest_segment_base();
    first.reset();
    check(fls_callback_values == std::vector<void *>{&first_value} && fls_callback_context,
          "guest context exit invokes its FLS callback while GS/TLS remains active");
    check(host::guest_segment_base() == original_gs,
          "FLS exit cleanup restores native GS after the final callback");
    second.reset();
    check(fls_callback_values.size() == 1, "null-valued context exit does not invoke FLS callback");
    check(slots.free(index) == 0, "allocated index remains freeable after all contexts exit");
    std::vector<std::uint32_t> exhausted;
    for (unsigned i = 0; i < 128; ++i) {
        auto entry = slots.allocate(nullptr);
        if (!entry.error) {
            exhausted.push_back(entry.index);
        }
    }
    check(exhausted.size() == 128 && slots.allocate(nullptr).index == fls_out_of_indexes,
          "FLS index budget is bounded and exhaustion uses the defined sentinel");
    for (auto entry : exhausted) {
        (void)slots.free(entry);
    }

    Runtime runtime;
    Scope runtime_scope(runtime);
    using Allocate = std::uint32_t(AMBROSIA_GUEST_ABI *)(FlsCallback);
    using Get = void *(AMBROSIA_GUEST_ABI *)(std::uint32_t);
    using Set = std::int32_t(AMBROSIA_GUEST_ABI *)(std::uint32_t, void *);
    using Free = std::int32_t(AMBROSIA_GUEST_ABI *)(std::uint32_t);
    const auto allocate = reinterpret_cast<Allocate>(runtime.resolve(service_symbol("FlsAlloc")));
    const auto get = reinterpret_cast<Get>(runtime.resolve(service_symbol("FlsGetValue")));
    const auto set = reinterpret_cast<Set>(runtime.resolve(service_symbol("FlsSetValue")));
    const auto free = reinterpret_cast<Free>(runtime.resolve(service_symbol("FlsFree")));
    {
        guest::ThreadContext context;
        auto active = context.activate();
        set_last_error(66);
        const auto slot = allocate(nullptr);
        check(slot != fls_out_of_indexes && get_last_error() == 66,
              "FlsAlloc ABI returns a slot while preserving successful last error");
        check(!get(slot) && get_last_error() == 0, "FlsGetValue clears last error for a valid null value");
        check(set(slot, &first_value) == 1 && get(slot) == &first_value,
              "FLS Microsoft ABI wrappers store and retrieve the actual pointer");
        check(free(slot) == 1 && !get(slot) && get_last_error() == error_invalid_parameter,
              "FLS ABI free invalidates the old slot and reports invalid get");
    }
    check(runtime.resolve(service_symbol("FlsAlloc", "api-ms-win-core-fibers-l1-1-0.dll")) ==
              reinterpret_cast<std::uintptr_t>(allocate),
          "observed FLS API-set module resolves to the native implementation");

    std::vector<unsigned> exit_order;
    {
        guest::ThreadContext context;
        context.on_exit([&] { exit_order.push_back(1); });
        context.on_exit([&] { exit_order.push_back(2); });
    }
    check(exit_order == std::vector<unsigned>{2, 1},
          "guest exit hooks run once in reverse registration order");
}
