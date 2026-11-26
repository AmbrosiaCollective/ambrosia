struct SortObservation {
    std::byte *base = nullptr;
    std::size_t count = 0;
    std::size_t width = 0;
    ambrosia::services::Runtime *runtime = nullptr;
    ambrosia::pe::ModuleGraph *graph = nullptr;
    ambrosia::guest::ThreadContext *thread = nullptr;
    std::thread::id host_thread;
    unsigned calls = 0;
    bool valid = true;
    bool byte_key = false;
    bool descending = false;
};

SortObservation *active_sort = nullptr;
unsigned sort_notifications = 0;
unsigned sort_invalid_calls = 0;

void AMBROSIA_GUEST_ABI sort_tls_notification(void *, std::uint32_t, void *) {
    ++sort_notifications;
}

void AMBROSIA_GUEST_ABI sort_invalid_parameter(const char16_t *, const char16_t *, const char16_t *,
                                               std::uint32_t, std::uintptr_t) {
    ++sort_invalid_calls;
}

std::int32_t AMBROSIA_GUEST_ABI compare_sort_records(const void *left, const void *right) {
    using namespace ambrosia;
    using namespace services;
    if (!active_sort) {
        return 0;
    }
    auto &observed = *active_sort;
    const auto member = [&](const void *pointer) {
        const auto address = reinterpret_cast<std::uintptr_t>(pointer);
        const auto base = reinterpret_cast<std::uintptr_t>(observed.base);
        return address >= base && address - base < observed.count * observed.width &&
               (address - base) % observed.width == 0;
    };
    const auto valid = member(left) && member(right) && current_runtime() == observed.runtime &&
                       current_modules() == observed.graph &&
                       guest::ThreadContext::current() == observed.thread &&
                       std::this_thread::get_id() == observed.host_thread;
    observed.valid = observed.valid && valid;
    if (!valid) {
        return 0;
    }
    auto *tls = current_guest_tls();
    observed.valid = observed.valid && tls[0] == 77 + observed.calls;
    ++observed.calls;
    ++tls[0];
    std::int64_t first = 0;
    std::int64_t second = 0;
    if (observed.byte_key) {
        first = *static_cast<const unsigned char *>(left);
        second = *static_cast<const unsigned char *>(right);
    } else {
        std::memcpy(&first, left, sizeof(first));
        std::memcpy(&second, right, sizeof(second));
    }
    // Guest callbacks may change guest errors. The adapter must retain those
    // changes while keeping the host's errno isolated from this fixture callback.
    crt_error() = 73;
    set_last_error(211);
    errno = ERANGE;
    auto result = first < second ? INT32_MIN : first > second ? INT32_MAX : 0;
    if (observed.descending) {
        result = first < second ? INT32_MAX : first > second ? INT32_MIN : 0;
    }
    return result;
}

void sort_checks() {
    using namespace ambrosia;
    using namespace services;
    using Compare = std::int32_t(AMBROSIA_GUEST_ABI *)(const void *, const void *);
    using Sort = void(AMBROSIA_GUEST_ABI *)(void *, std::size_t, std::size_t, Compare);
    Runtime runtime;
    const auto address = runtime.resolve(service_symbol("qsort", "ucrtbase.dll"));
    check(address && address == runtime.resolve(service_symbol("qsort", "api-ms-win-crt-utility-l1-1-0.dll")),
          "CRT qsort aliases expose a typed Windows x64 entrypoint");
    if (!address) {
        return;
    }
    const auto sort = reinterpret_cast<Sort>(address);
    auto bytes = tls_fixture();
    put<std::uint32_t>(bytes, 0x98 + 120, 0);
    put<std::uint32_t>(bytes, 0x98 + 124, 0);
    jump_to_host(bytes, 0x300, reinterpret_cast<std::uintptr_t>(&sort_tls_notification));
    jump_to_host(bytes, 0x380, reinterpret_cast<std::uintptr_t>(&compare_sort_records));
    pe::ModuleGraph graph;
    graph.add("sorting.exe", bytes);
    graph.load("sorting.exe");
    Scope scope(runtime, &graph);
    sort_notifications = 0;
    auto process = graph.start();
    const auto compare = reinterpret_cast<Compare>(graph.loaded_module() + 0x1180);
    process.with_guest_context([&] {
        SortObservation observed;
        observed.runtime = &runtime;
        observed.graph = &graph;
        observed.thread = guest::ThreadContext::current();
        observed.host_thread = std::this_thread::get_id();
        active_sort = &observed;
        const auto reset = [&](void *base, std::size_t count, std::size_t width) {
            observed.base = static_cast<std::byte *>(base);
            observed.count = count;
            observed.width = width;
            observed.calls = 0;
            observed.valid = true;
            current_guest_tls()[0] = 77;
        };
        const auto notifications = sort_notifications;
        struct Record {
            std::int64_t key;
            std::uint64_t payload;
            bool operator==(const Record &) const = default;
        };
        static_assert(sizeof(Record) == 16);
        std::array<Record, 48> records{};
        for (std::size_t index = 0; index < records.size(); ++index) {
            records[index] = {static_cast<std::int64_t>((index * 17) % 13) - 6, 0x1000 + index};
        }
        const auto original = records;
        reset(records.data(), records.size(), sizeof(Record));
        const auto saved_errno = errno;
        errno = E2BIG;
        sort(records.data(), records.size(), sizeof(Record), compare);
        check(std::is_sorted(records.begin(), records.end(),
                             [](const auto &left, const auto &right) { return left.key < right.key; }) &&
                  observed.calls > 0,
              "the observed48 by16-byte workload sorts real original keys through a guest comparator");
        check(observed.valid && current_guest_tls()[0] == 77 + observed.calls &&
                  sort_notifications == notifications,
              "comparison arguments stay at array elements and retain caller TLS, module scope and thread");
        check(errno == E2BIG && crt_error() == 73 && get_last_error() == 211,
              "qsort preserves host errno while retaining changes made by the guest comparator");
        auto by_payload = records;
        std::sort(by_payload.begin(), by_payload.end(),
                  [](const auto &left, const auto &right) { return left.payload < right.payload; });
        check(by_payload == original,
              "record moves preserve every key/payload association, including duplicate comparison keys");
        std::array<std::int64_t, 7> integers{INT64_MAX, 4, -1, INT64_MIN, 4, 0, -9};
        const auto before_integers = integers;
        reset(integers.data(), integers.size(), sizeof(std::int64_t));
        observed.descending = true;
        sort(integers.data(), integers.size(), sizeof(std::int64_t), compare);
        check(integers == std::array<std::int64_t, 7>{INT64_MAX, 4, 4, 0, -1, -9, INT64_MIN} &&
                  observed.valid,
              "descending order uses only comparator sign, including extreme signed return values");
        observed.descending = false;
        reset(integers.data(), integers.size(), sizeof(std::int64_t));
        sort(integers.data(), integers.size(), sizeof(std::int64_t), compare);
        check(std::is_sorted(integers.begin(), integers.end()) && observed.valid,
              "already ordered and duplicate signed64 records sort in either comparison direction");
        std::array<unsigned char, 17> packed{0xaa, 4, 10, 20, 0, 11, 21, 255, 12,
                                             22,   4, 13, 23, 1, 14, 24, 0xaa};
        const auto original_packed = packed;
        observed.byte_key = true;
        reset(packed.data() + 1, 5, 3);
        sort(packed.data() + 1, 5, 3, compare);
        const auto collect = [](const auto &buffer) {
            std::array<std::array<unsigned char, 3>, 5> result{};
            for (std::size_t index = 0; index < result.size(); ++index) {
                std::copy_n(buffer.data() + 1 + index * 3, 3, result[index].begin());
            }
            std::sort(result.begin(), result.end());
            return result;
        };
        check(packed.front() == 0xaa && packed.back() == 0xaa && observed.valid && packed[1] == 0 &&
                  packed[4] == 1 && packed[7] == 4 && packed[10] == 4 && packed[13] == 255 &&
                  collect(packed) == collect(original_packed),
              "unaligned three-byte records retain all bytes and outer guards without word-size assumptions");
        observed.byte_key = false;
        reset(integers.data(), integers.size(), sizeof(std::int64_t));
        const auto sorted_integers = integers;
        sort(nullptr, 0, sizeof(std::int64_t), compare);
        sort(integers.data(), 1, sizeof(std::int64_t), compare);
        check(observed.calls == 0 && integers == sorted_integers && current_guest_tls()[0] == 77,
              "zero and one-element sorts do not call the comparator or alter input");
        const auto previous_handler = runtime.invalid_parameters().replace(&sort_invalid_parameter);
        sort_invalid_calls = 0;
        sort(nullptr, 2, sizeof(std::int64_t), compare);
        sort(integers.data(), integers.size(), 0, compare);
        sort(integers.data(), integers.size(), sizeof(std::int64_t), nullptr);
        runtime.invalid_parameters().replace(previous_handler);
        check(sort_invalid_calls == 3 && crt_error() == 22 && observed.calls == 0 &&
                  integers == sorted_integers,
              "invalid qsort base, width and comparator dispatch EINVAL and leave records untouched");
        check(std::is_permutation(integers.begin(), integers.end(), before_integers.begin()) &&
                  sort_notifications == notifications,
              "sorting never attaches a replacement guest context or loses an original record");
        active_sort = nullptr;
        errno = saved_errno;
    });
}

int sort_rejection_probe(unsigned mode) {
    using namespace ambrosia;
    using namespace services;
    using Compare = std::int32_t(AMBROSIA_GUEST_ABI *)(const void *, const void *);
    using Sort = void(AMBROSIA_GUEST_ABI *)(void *, std::size_t, std::size_t, Compare);
    Runtime runtime;
    Scope scope(runtime);
    guest::ThreadContext context;
    auto activation = context.activate();
    const auto sort = reinterpret_cast<Sort>(runtime.resolve(service_symbol("qsort", "ucrtbase.dll")));
    std::array<std::int64_t, 2> records{2, 1};
    if (mode == 1) {
        sort(records.data(), 1024 * 1024 + 1, 1, &compare_sort_records);
    } else if (mode == 2) {
        sort(records.data(), 2, 32 * 1024 * 1024 + 1, &compare_sort_records);
    } else if (mode == 3) {
        sort(reinterpret_cast<void *>(std::numeric_limits<std::uintptr_t>::max() - 1), 2, 1,
             &compare_sort_records);
    } else if (mode == 4) {
        sort(records.data(), records.size(), sizeof(std::int64_t), &compare_sort_records);
    } else {
        return 2;
    }
    return 1;
}
