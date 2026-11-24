void crt_stream_checks() {
    using namespace ambrosia::services;
    Runtime first;
    Runtime second;
    using Accessor = void *(AMBROSIA_GUEST_ABI *)(unsigned);
    const auto get_file = reinterpret_cast<Accessor>(
        first.resolve(service_symbol("__acrt_iob_func", "api-ms-win-crt-stdio-l1-1-0.dll")));
    void *saved = nullptr;
    {
        Scope scope(first);
        set_last_error(91);
        auto *input = get_file(0);
        auto *output = get_file(1);
        auto *error = get_file(2);
        saved = output;
        check(input && output && error && input != output && output != error && input != error,
              "UCRT standard streams have distinct non-null guest FILE identities");
        check(first.streams().index(input) == 0 && first.streams().index(output) == 1 &&
                  first.streams().index(error) == 2,
              "guest FILE identities resolve to stdin/stdout/stderr indices");
        check(get_file(1) == output && get_last_error() == 91,
              "UCRT accessor preserves identity and last error across repeated calls");
        {
            Scope nested(second);
            check(get_file(1) != output && !second.streams().index(output).has_value(),
                  "separate runtime rejects another runtime's FILE identity");
        }
        check(get_file(1) == saved, "nested runtime restores the original standard streams");
    }
    std::uint64_t foreign = 0;
    check(!first.streams().index(&foreign).has_value() && !first.streams().index(nullptr).has_value(),
          "foreign and null FILE pointers are not followed or accepted");
    check(!first.streams().file(3) && !first.streams().file(UINT32_MAX),
          "standard-stream index is bounded to the three defined identities");
}
