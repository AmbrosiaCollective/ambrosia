// Original tiny PE byte fixtures only. No SDK, game, driver or network needed.
#include "clock_services.h"
#include "event_signal.h"
#include "guest_thread.h"
#include "host_affinity.h"
#include "host_priority.h"
#include "module_graph.h"
#include "native_command.h"
#include "package_modules.h"
#include "pe_bytes.h"
#include "pe_image.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include "unwind_epilogue.h"
#include "unwind_program.h"
#include "unwind_table.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cfenv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {
using Bytes = std::vector<std::byte>;
unsigned checks = 0, failures = 0;
bool saw_arguments = false;

void check(bool condition, const char *what) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << what << '\n';
    }
}

template <class T> void put(Bytes &bytes, std::size_t offset, T value) {
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        bytes.at(offset + i) = std::byte((std::uint64_t(value) >> (8 * i)) & 255);
    }
}

void text(Bytes &bytes, std::size_t offset, const char *value) {
    std::memcpy(bytes.data() + offset, value, std::strlen(value) + 1);
}

Bytes fixture() {
    Bytes bytes(0x800);
    put<std::uint16_t>(bytes, 0, 0x5a4d);
    put<std::uint32_t>(bytes, 0x3c, 0x80);
    put<std::uint32_t>(bytes, 0x80, 0x4550);
    put<std::uint16_t>(bytes, 0x84, 0x8664);
    put<std::uint16_t>(bytes, 0x86, 3);
    put<std::uint16_t>(bytes, 0x94, 240);
    put<std::uint16_t>(bytes, 0x96, 0x22);
    constexpr std::size_t optional = 0x98;
    put<std::uint16_t>(bytes, optional, 0x20b);
    put<std::uint32_t>(bytes, optional + 16, 0x1000);
    put<std::uint64_t>(bytes, optional + 24, 0x180000000ull);
    put<std::uint32_t>(bytes, optional + 32, 0x1000);
    put<std::uint32_t>(bytes, optional + 36, 0x200);
    put<std::uint32_t>(bytes, optional + 56, 0x4000);
    put<std::uint32_t>(bytes, optional + 60, 0x200);
    put<std::uint16_t>(bytes, optional + 68, 3);
    put<std::uint32_t>(bytes, optional + 108, 16);
    put<std::uint32_t>(bytes, optional + 120, 0x2000); // import directory
    put<std::uint32_t>(bytes, optional + 124, 40);
    put<std::uint32_t>(bytes, optional + 152, 0x3000); // relocation directory
    put<std::uint32_t>(bytes, optional + 156, 12);
    for (unsigned i = 0; i < 3; ++i) {
        const auto section = 0x188 + i * 40;
        put<std::uint32_t>(bytes, section + 8, 0x200);
        put<std::uint32_t>(bytes, section + 12, 0x1000 + i * 0x1000);
        put<std::uint32_t>(bytes, section + 16, 0x200);
        put<std::uint32_t>(bytes, section + 20, 0x200 + i * 0x200);
        put<std::uint32_t>(bytes, section + 36, i == 0 ? 0x60000020 : i == 1 ? 0xc0000040 : 0x40000040);
    }
    // Microsoft x64 ABI: four register arguments, two stack arguments,
    // 32-byte shadow space and 16-byte alignment at the call instruction.
    constexpr unsigned char code[] = {0x48, 0x83, 0xec, 0x38, 0xb9, 1,    0,    0,    0,    0xba, 2,
                                      0,    0,    0,    0x41, 0xb8, 3,    0,    0,    0,    0x41, 0xb9,
                                      4,    0,    0,    0,    0x48, 0xc7, 0x44, 0x24, 0x20, 5,    0,
                                      0,    0,    0x48, 0xc7, 0x44, 0x24, 0x28, 6,    0,    0,    0,
                                      0xff, 0x15, 0,    0,    0,    0,    0x48, 0x83, 0xc4, 0x38, 0xc3};
    std::memcpy(bytes.data() + 0x200, code, sizeof(code));
    put<std::uint32_t>(bytes, 0x200 + 46, 0x2080 - (0x1000 + 50));
    put<std::uint32_t>(bytes, 0x400, 0x2060); // OriginalFirstThunk
    put<std::uint32_t>(bytes, 0x40c, 0x2040); // module name
    put<std::uint32_t>(bytes, 0x410, 0x2080); // FirstThunk
    text(bytes, 0x440, "ambrosia_fixture.dll");
    put<std::uint64_t>(bytes, 0x460, 0x20a0);
    text(bytes, 0x4a2, "sum6");
    put<std::uint64_t>(bytes, 0x500, 0x180001000ull);
    put<std::uint32_t>(bytes, 0x600, 0x2000);
    put<std::uint32_t>(bytes, 0x604, 12);
    put<std::uint16_t>(bytes, 0x608, 0xa100);
    return bytes;
}

std::uint64_t AMBROSIA_GUEST_ABI sum6(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d,
                                      std::uint64_t e, std::uint64_t f) {
    saw_arguments = a == 1 && b == 2 && c == 3 && d == 4 && e == 5 && f == 6;
    return a + b + c + d + e + f;
}

std::uintptr_t resolve(const ambrosia::pe::Import &import) {
    if (import.module == "ambrosia_fixture.dll" &&
        (import.by_ordinal ? import.ordinal == 7 : import.name == "sum6")) {
        return reinterpret_cast<std::uintptr_t>(&sum6);
    }
    return 0;
}

bool rejected(const Bytes &bytes, const ambrosia::pe::Resolver &resolver = resolve) {
    try {
        auto image = ambrosia::pe::Image::load(bytes, resolver);
    } catch (const std::runtime_error &) {
        return true;
    }
    return false;
}

template <class Function> void bad(Function mutation, const char *label) {
    auto bytes = fixture();
    mutation(bytes);
    check(rejected(bytes), label);
}

template <class Function> bool runtime_rejected(Function operation) {
    try {
        operation();
    } catch (const std::runtime_error &) {
        return true;
    }
    return false;
}

// These fixture fragments depend on helpers defined by the preceding fragment.
// clang-format off
#include "lifecycle_tests.h"
#include "module_tests.h"
#include "package_tests.h"
#include "thread_tests.h"
#include "service_tests.h"
#include "limit_tests.h"
#include "unwind_tests.h"
#include "inspection_tests.h"
#include "unwind_program_tests.h"
#include "unwind_walk_tests.h"
#include "trace_tests.h"
#include "clock_tests.h"
#include "process_handle_tests.h"
#include "critical_tests.h"
#include "slim_lock_tests.h"
#include "fls_tests.h"
#include "tib_tests.h"
#include "crt_tests.h"
#include "slist_tests.h"
#include "loader_tests.h"
#include "crt_stream_tests.h"
#include "crt_heap_tests.h"
#include "locale_tests.h"
#include "pointer_tests.h"
#include "stream_buffer_tests.h"
#include "c_locale_tests.h"
#include "module_address_tests.h"
#include "crt_startup_tests.h"
#include "crt_exit_tests.h"
#include "context_locale_tests.h"
#include "module_handle_tests.h"
#include "module_path_tests.h"
#include "dynamic_library_tests.h"
#include "gdk_bootstrap_tests.h"
#include "gdk_threading_tests.h"
#include "task_queue_tests.h"
#include "app_lifecycle_tests.h"
#include "gdk_networking_tests.h"
#include "winsock_tests.h"
#include "worker_module_tests.h"
#include "native_thread_tests.h"
#include "thread_priority_tests.h"
#include "thread_affinity_tests.h"
#include "open_thread_tests.h"
#include "priority_boost_tests.h"
#include "native_module_tests.h"
#include "event_tests.h"
#include "registry_tests.h"
#include "graphics_memory_tests.h"
#include "xmem_tests.h"
#include "memory_budget_tests.h"
#include "xmem_committed_tests.h"
#include "semaphore_tests.h"
#include "mutex_tests.h"
#include "private_memory_option_tests.h"
#include "processor_topology_tests.h"
#include "exception_filter_tests.h"
#include "new_allocation_tests.h"
#include "math_tests.h"
#include "command_line_tests.h"
#include "string_tests.h"
#include "format_tests.h"
#include "wide_format_tests.h"
#include "invalid_parameter_tests.h"
#include "random_tests.h"
// clang-format on

void pe_checks() {
    const auto bytes = fixture();
    auto image = ambrosia::pe::Image::load(bytes, resolve);
    check(image.run_bootstrap() == 21, "bootstrap returns imported function result");
    check(saw_arguments, "Microsoft x64 register and stack arguments");
    check(image.imports().size() == 1 && image.imports()[0].name == "sum6", "named import evidence");
    std::uint64_t pointer = 0;
    std::memcpy(&pointer, image.base() + 0x2100, sizeof(pointer));
    check(pointer == reinterpret_cast<std::uintptr_t>(image.base() + 0x1000),
          "DIR64 rebases an absolute address");
    check(image.base()[0x2200] == std::byte{0}, "virtual section tail is zero-filled");
    auto moved = std::move(image);
    check(moved.run_bootstrap() == 21, "mapping ownership survives move");
    bool moved_rejected = false;
    try {
        (void)image.run_bootstrap();
    } catch (const std::runtime_error &) {
        moved_rejected = true;
    }
    check(moved_rejected, "moved-from image cannot execute");
    auto ordinal = fixture();
    put<std::uint64_t>(ordinal, 0x460, 0x8000000000000007ull);
    auto ordinal_image = ambrosia::pe::Image::load(ordinal, resolve);
    check(ordinal_image.run_bootstrap() == 21 && ordinal_image.imports()[0].by_ordinal,
          "ordinal import dispatch");
    auto fallback = fixture();
    put<std::uint32_t>(fallback, 0x400, 0);
    put<std::uint64_t>(fallback, 0x480, 0x20a0);
    check(ambrosia::pe::Image::load(fallback, resolve).run_bootstrap() == 21, "FirstThunk lookup fallback");
    check(rejected(bytes, {}), "unresolved import fails before execution");
    bool truncated_rejected = true;
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        if (!rejected(Bytes(bytes.begin(), bytes.begin() + length))) {
            truncated_rejected = false;
            break;
        }
    }
    check(truncated_rejected, "every shorter fixture length is rejected");
    bad([](auto &b) { put<std::uint16_t>(b, 0x84, 0xaa64); }, "non-x64 image rejected");
    bad([](auto &b) { put<std::uint32_t>(b, 0x3c, 0xfffffff0); }, "bad NT header offset rejected");
    bad([](auto &b) { put<std::uint32_t>(b, 0x98 + 108, 17); }, "directory count overflow rejected");
    bad([](auto &b) { put<std::uint32_t>(b, 0x98 + 16, 0x2000); }, "non-executable entry rejected");
    bad([](auto &b) { put<std::uint32_t>(b, 0x188 + 36, 0xe0000020); }, "writable executable page rejected");
    bad([](auto &b) { put<std::uint32_t>(b, 0x1b0 + 12, 0x1000); }, "overlapping virtual sections rejected");
    bad([](auto &b) { put<std::uint32_t>(b, 0x1b0 + 20, 0x200); }, "overlapping raw sections rejected");
    bad([](auto &b) { put<std::uint32_t>(b, 0x98 + 32, 512); }, "sub-page section alignment rejected");
    bad([](auto &b) { put<std::uint32_t>(b, 0x98 + 56, 0xfffff000); }, "oversized image rejected");
    bad([](auto &b) { put<std::uint32_t>(b, 0x98 + 152, 0); }, "incomplete directory rejected");
    bad([](auto &b) { put<std::uint32_t>(b, 0x604, 7); }, "short relocation block rejected");
    bad([](auto &b) { put<std::uint16_t>(b, 0x608, 0x3100); }, "unsupported relocation rejected");
    bad([](auto &b) { put<std::uint32_t>(b, 0x600, 0x4000); }, "relocation destination bounds enforced");
    bad([](auto &b) { put<std::uint16_t>(b, 0x96, 0x23); }, "stripped relocations rejected");
    bad(
        [](auto &b) {
            put<std::uint32_t>(b, 0x98 + 184, 0x2100);
            put<std::uint32_t>(b, 0x98 + 188, 40);
        },
        "TLS cannot be silently skipped");
    bad(
        [](auto &b) {
            put<std::uint32_t>(b, 0x98 + 136, 0x2100);
            put<std::uint32_t>(b, 0x98 + 140, 12);
        },
        "exception registration cannot be skipped");
    bad([](auto &b) { put<std::uint32_t>(b, 0x98 + 124, 20); },
        "missing import descriptor terminator rejected");
    bad([](auto &b) { put<std::uint32_t>(b, 0x410, 0x3ffc); }, "IAT bounds enforced");
    bad([](auto &b) { put<std::uint64_t>(b, 0x460, 0xffffffffffffffffull); }, "invalid ordinal rejected");
    bad([](auto &b) { text(b, 0x440, "../fixture.dll"); }, "import traversal rejected");
    bad([](auto &b) { text(b, 0x4a2, "unknown"); }, "unknown named import rejected");
}

void host_checks() {
    using ambrosia::host::Mapping;
    Mapping memory(Mapping::page_size() + 1);
    check(memory.size() == Mapping::page_size() * 2, "mapping uses runtime page size");
    bool rejected_range = false;
    try {
        memory.protect(1, memory.size(), ambrosia::host::Access::read);
    } catch (const std::runtime_error &) {
        rejected_range = true;
    }
    check(rejected_range, "unaligned protection rejected");
    auto moved = std::move(memory);
    check(!memory.data() && moved.data(), "host mapping moves without double ownership");
    const auto root = std::filesystem::temp_directory_path() / "ambrosia-native-fixtures";
    std::filesystem::create_directories(root / "package");

    struct Cleanup {
        std::filesystem::path root;

        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
        }
    } cleanup{root};

    const auto image = root / "package" / std::filesystem::path(u8"fixture café.bin");
    {
        std::ofstream output(image);
        output << "synthetic";
    }
    const auto cwd = std::filesystem::current_path();
    const auto paths = ambrosia::host::resolve_paths(image, {}, root / "work root");
    check(paths.image == std::filesystem::canonical(image) && paths.package == image.parent_path(),
          "Unicode input and package path resolution");
    check(paths.work.is_absolute() && !std::filesystem::exists(paths.work),
          "work root resolved without writes");
    check(std::filesystem::current_path() == cwd, "path resolution does not change cwd");
    check(ambrosia::host::resolve_paths(image).work.is_absolute(),
          "default work root determined dynamically");
    bool nested_rejected = false;
    try {
        (void)ambrosia::host::resolve_paths(image, {}, image.parent_path() / "cache");
    } catch (const std::runtime_error &) {
        nested_rejected = true;
    }
    check(nested_rejected, "work inside package rejected");
    bool missing_rejected = false;
    try {
        (void)ambrosia::host::resolve_paths(root / "absent");
    } catch (const std::runtime_error &) {
        missing_rejected = true;
    }
    check(missing_rejected, "missing executable rejected");
}
} // namespace

extern "C" int ambrosia_native_invalid_parameter_probe(unsigned mode) {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    Scope scope(runtime);
    guest::ThreadContext thread;
    auto activation = thread.activate();
    if (mode == 0) {
        dispatch_invalid_parameter();
    } else if (mode == 1) {
        InvalidParameterRecord record;
        active_invalid_record = &record;
        runtime.invalid_parameters().replace(&global_invalid_parameter);
        using Fatal = void(AMBROSIA_GUEST_ABI *)();
        const auto fatal = reinterpret_cast<Fatal>(
            runtime.resolve(service_symbol("_invalid_parameter_noinfo_noreturn", "ucrtbase.dll")));
        fatal();
    }
    return 99;
}

extern "C" int ambrosia_native_wide_format_probe(unsigned mode) {
    using namespace ambrosia::services;
    Runtime runtime;
    Scope scope(runtime);
    using Format = std::int32_t(AMBROSIA_GUEST_ABI *)(std::uint64_t, char16_t *, std::size_t,
                                                      const char16_t *, void *, const std::byte *);
    const auto format = reinterpret_cast<Format>(
        runtime.resolve(service_symbol("__stdio_common_vswprintf_s", "ucrtbase.dll")));
    std::array<char16_t, 8> output{};
    std::array<std::uint64_t, 2> slots{0, 0};
    const char16_t *pattern = u"abc";
    const unsigned char non_ascii[] = {0xc3, 0xa9, 0};
    std::size_t size = output.size();
    std::uint64_t options = 36;
    void *locale = nullptr;
    switch (mode) {
    case 0:
        size = 3;
        break;
    case 1:
        pattern = u"%s";
        break;
    case 2:
        pattern = u"%n";
        break;
    case 3:
        options = 38;
        break;
    case 4:
        locale = reinterpret_cast<void *>(1);
        break;
    case 5:
        pattern = u"%S";
        slots[0] = reinterpret_cast<std::uintptr_t>(non_ascii);
        break;
    default:
        return 99;
    }
    return format(options, output.data(), size, pattern, locale,
                  reinterpret_cast<const std::byte *>(slots.data()));
}

extern "C" int ambrosia_native_thread_tests() {
    checks = failures = 0;
    try {
        native_thread_handle_checks();
        native_thread_launcher_checks();
        native_suspended_thread_checks();
        native_thread_priority_checks();
        native_thread_affinity_checks();
        open_thread_checks();
        thread_priority_boost_checks();
    } catch (const std::exception &error) {
        std::cerr << "FAIL native thread exception: " << error.what() << '\n';
        ++failures;
    }
    std::cout << checks << " native thread checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}

extern "C" int ambrosia_native_thread_probe(unsigned mode) {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    auto bytes = native_thread_fixture();
    if (mode == 1) {
        bytes[0x380] = std::byte{0x0f};
        bytes[0x381] = std::byte{0x0b};
    }
    pe::ModuleGraph graph([&](const auto &symbol) { return runtime.resolve(symbol); });
    graph.add("thread-fault.exe", bytes);
    graph.load("thread-fault.exe");
    Scope scope(runtime, &graph);
    diagnostics::FaultScope faults(graph.diagnostic_ranges());
    auto process = graph.start();
    using Create =
        std::uintptr_t(AMBROSIA_GUEST_ABI *)(void *, std::size_t, ThreadEntry, void *, std::uint32_t, void *);
    const auto create =
        reinterpret_cast<Create>(runtime.resolve(service_symbol("CreateThread", "kernel32.dll")));
    const auto entry = reinterpret_cast<ThreadEntry>(graph.loaded_module() + 0x1180);
    std::uint32_t id = 0xfaceb00c;
    const auto handle = create(nullptr, 65536, entry, reinterpret_cast<void *>(23),
                               mode == 2   ? 8
                               : mode == 3 ? 4
                                           : 0,
                               &id);
    const auto error = get_last_error();
    if (handle && mode != 3) {
        (void)runtime.objects().wait(handle, 2000);
        (void)runtime.objects().close(handle);
    }
    runtime.threads().shutdown();
    process.close();
    return mode == 0 && !handle && error == 8 && id == 0xfaceb00c ? 0 : 99;
}

extern "C" int ambrosia_native_worker_module_tests() {
    checks = failures = 0;
    try {
        worker_module_checks();
    } catch (const std::exception &error) {
        std::cerr << "FAIL worker module exception: " << error.what() << '\n';
        ++failures;
    }
    std::cout << checks << " worker module checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}

extern "C" int ambrosia_native_networking_probe() {
    using namespace ambrosia::services;
    Runtime runtime;
    Scope scope(runtime);
    (void)runtime.gdk_bootstrap().initialize(1, 2);
    void *object = nullptr;
    (void)runtime.gdk_networking().query(networking_v2_id, &object);
    const auto *table = *static_cast<const std::uintptr_t *const *>(object);
    using Unsupported = void(AMBROSIA_GUEST_ABI *)();
    reinterpret_cast<Unsupported>(table[16])();
    return 99;
}

extern "C" int ambrosia_native_lifecycle_thread_tests() {
    checks = failures = 0;
    try {
        app_lifecycle_thread_checks();
    } catch (const std::exception &error) {
        std::cerr << "FAIL lifecycle thread exception: " << error.what() << '\n';
        ++failures;
    }
    std::cout << checks << " lifecycle thread checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}

extern "C" int ambrosia_native_queue_tests() {
    checks = failures = 0;
    try {
        task_queue_manual_checks();
        worker_pool_checks();
        task_queue_thread_checks();
    } catch (const std::exception &error) {
        std::cerr << "FAIL queue exception: " << error.what() << '\n';
        ++failures;
    }
    std::cout << checks << " queue checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}

extern "C" int ambrosia_native_queue_denied_probe() {
    using namespace ambrosia::services;
    Runtime runtime;
    Scope scope(runtime);
    std::uintptr_t handle = 9;
    const auto result = task_queue_create(1, 1, &handle);
    return result == gdk_out_of_memory && !handle && !runtime.task_queues().workers() ? 0 : 1;
}

extern "C" int ambrosia_native_tests() {
    checks = failures = 0;
    try {
        host_checks();
        thread_checks();
        pe_checks();
        lifecycle_checks();
        export_checks();
        forwarder_checks();
        graph_tls_checks();
        package_checks();
        memory_service_checks();
        service_dispatch_checks();
        limit_checks();
        unwind_checks();
        inspection_checks();
        unwind_program_checks();
        unwind_walk_checks();
        trace_configuration_checks();
        clock_service_checks();
        current_process_handle_checks();
        process_priority_boost_checks();
        crt_time_checks();
        uptime_checks();
        critical_section_checks();
        slim_lock_checks();
        fls_checks();
        tib_checks();
        crt_lifecycle_checks();
        slist_checks();
        loader_policy_checks();
        crt_stream_checks();
        crt_heap_checks();
        locale_lock_checks();
        pointer_codec_checks();
        stream_buffer_checks();
        c_locale_checks();
        module_address_checks();
        crt_startup_checks();
        startup_info_checks();
        crt_exit_checks();
        executable_tls_exit_checks();
        context_locale_checks();
        module_handle_checks();
        module_path_checks();
        dynamic_library_checks();
        gdk_bootstrap_checks();
        runtime_interface_checks();
        gdk_threading_checks();
        task_queue_manual_checks();
        app_lifecycle_checks();
        gdk_networking_checks();
        ordinal_service_checks();
        winsock_lifecycle_checks();
        native_module_checks();
        event_checks();
        registry_checks();
        graphics_memory_checks();
        xmem_checks();
        memory_budget_checks();
        xmem_committed_checks();
        semaphore_checks();
        mutex_checks();
        processor_topology_checks();
        affinity_mapping_checks();
        exception_filter_checks();
        new_allocation_checks();
        tangent_checks();
        math_family_checks();
        command_line_checks();
        string_service_checks();
        format_checks();
        wide_format_checks();
        invalid_parameter_checks();
        secure_random_checks();
        private_memory_option_checks();
    } catch (const std::exception &error) {
        ++failures;
        std::cerr << "unexpected: " << error.what() << '\n';
    }
    std::cout << "{\"checks\":" << checks << ",\"failures\":" << failures << ",\"skipped\":0}\n";
    return failures ? 1 : 0;
}

// Inspection-only bridge for the strict in-process launcher. It never selects
// bootstrap mode or invokes initialization/entry callbacks.
extern "C" int ambrosia_native_inspect(const char *image, const char *package) {
    if (!image || !package) {
        return 2;
    }
    try {
        const std::vector<std::filesystem::path> arguments{
            "--inspect", std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(image))),
            "--package", std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(package)))};
        return ambrosia::run_native_command(arguments, std::cout, std::cerr);
    } catch (...) {
        std::cerr << "inspection probe argument construction failed\n";
        return 2;
    }
}

int trace_native_image(const char *image, const char *package, unsigned memory_profile) {
    if (!image || !package) {
        return 2;
    }
    try {
        std::vector<std::filesystem::path> arguments{
            "--trace-startup", std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(image))),
            "--package", std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(package)))};
        if (memory_profile == 1) {
            arguments.emplace_back("--sparse-windows");
        } else if (memory_profile == 2) {
            arguments.emplace_back("--boot-memory");
        } else if (memory_profile == 3) {
            arguments.emplace_back("--sparse-windows");
            arguments.emplace_back("--private-memory-mib");
            arguments.emplace_back("8192");
        }
        return ambrosia::run_native_command(arguments, std::cout, std::cerr);
    } catch (...) {
        std::cerr << "startup probe argument construction failed\n";
        return 2;
    }
}

extern "C" int ambrosia_native_trace(const char *image, const char *package) {
    return trace_native_image(image, package, 0);
}

extern "C" int ambrosia_native_gdk_threading_probe(unsigned mode) {
    using namespace ambrosia::services;
    Runtime runtime;
    Scope scope(runtime);
    ambrosia::guest::ThreadContext context;
    auto activation = context.activate();
    void *object = nullptr;
    (void)runtime.gdk_threading().query(threading_class_id, &object);
    const auto *table = *static_cast<const std::uintptr_t *const *>(object);
    if (mode == 0) {
        using Stop = void(AMBROSIA_GUEST_ABI *)();
        reinterpret_cast<Stop>(table[12])();
    } else if (mode == 1) {
        using Set = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, bool);
        using Assert = void(AMBROSIA_GUEST_ABI *)(void *);
        (void)reinterpret_cast<Set>(table[27])(object, true);
        reinterpret_cast<Assert>(table[29])(object);
    } else {
        for (unsigned i = 0; i < 4096; ++i) {
            (void)interface_add_reference(object);
        }
    }
    return 99;
}

extern "C" int ambrosia_native_gdk_probe(unsigned mode) {
    using namespace ambrosia::services;
    Runtime runtime;
    Scope scope(runtime);
    using Init = std::int32_t(AMBROSIA_GUEST_ABI *)(std::uint32_t, std::uint32_t, std::uint8_t, const void *);
    const auto init =
        reinterpret_cast<Init>(runtime.resolve(service_symbol("InitializeApiImplEx2", "xgameruntime.dll")));
    if (mode < 2) {
        const std::uint64_t options[2]{};
        (void)init(1, 2, mode == 0 ? 1 : 2, mode == 1 ? options : nullptr);
    } else {
        if (mode == 2) {
            (void)init(1, 2, 2, nullptr);
        }
        using Query = std::int32_t(AMBROSIA_GUEST_ABI *)(const void *, const void *, void **);
        const auto query =
            reinterpret_cast<Query>(runtime.resolve(service_symbol("QueryApiImpl", "xgameruntime.dll")));
        const std::uint64_t guid[2]{};
        void *object = nullptr;
        (void)query(guid, guid, &object);
    }
    return 99;
}

extern "C" int ambrosia_native_random_probe(unsigned mode) {
    using namespace ambrosia::services;
    Runtime runtime;
    Scope scope(runtime);
    using Random = std::int32_t(AMBROSIA_GUEST_ABI *)(void *);
    const auto random = reinterpret_cast<Random>(runtime.resolve(service_symbol("rand_s", "ucrtbase.dll")));
    std::uint32_t value = 0;
    (void)random(mode ? &value : nullptr);
    return 99;
}

extern "C" int ambrosia_native_invalid_format(unsigned mode) {
    using namespace ambrosia::services;
    Runtime runtime;
    Scope scope(runtime);
    using Format = std::int32_t(AMBROSIA_GUEST_ABI *)(std::uint64_t, char *, std::size_t, const char *,
                                                      void *, const std::byte *);
    const auto format =
        reinterpret_cast<Format>(runtime.resolve(service_symbol("__stdio_common_vsprintf", "ucrtbase.dll")));
    char buffer[8]{};
    char16_t wide[] = {0xe9, 0};
    std::uint64_t slot = reinterpret_cast<std::uintptr_t>(wide);
    const auto options = mode == 0 ? 8 : mode == 6 ? 32 : 1;
    const char *pattern = "abc";
    if (mode == 2) {
        pattern = "%n";
    } else if (mode == 3) {
        pattern = "%ls";
    } else if (mode == 4) {
        pattern = "%d";
    } else if (mode == 5 || mode == 6) {
        pattern = "%.0f";
        slot = std::bit_cast<std::uint64_t>(2.5);
        if (mode == 6 && std::fesetround(FE_UPWARD)) {
            return 98;
        }
    }
    (void)format(options, buffer, mode == 1 ? 1 : sizeof(buffer), pattern, nullptr,
                 mode == 4 ? nullptr : reinterpret_cast<const std::byte *>(&slot));
    return 99; // Every selected unsupported path must stop instead of returning.
}

extern "C" int ambrosia_native_invalid_new_mode() {
    ambrosia::services::Runtime runtime;
    ambrosia::services::Scope scope(runtime);
    using SetMode = std::int32_t(AMBROSIA_GUEST_ABI *)(std::int32_t);
    const auto set_mode =
        reinterpret_cast<SetMode>(runtime.resolve(service_symbol("_set_new_mode", "ucrtbase.dll")));
    return set_mode(2);
}

extern "C" int ambrosia_native_trace_sparse(const char *image, const char *package) {
    return trace_native_image(image, package, 1);
}

extern "C" int ambrosia_native_trace_boot_memory(const char *image, const char *package) {
    return trace_native_image(image, package, 2);
}

extern "C" int ambrosia_native_trace_memory_8g(const char *image, const char *package) {
    return trace_native_image(image, package, 3);
}

extern "C" int ambrosia_native_trace_fixture(unsigned scenario) {
    auto bytes = fixture();
    if (scenario == 1) {
        bytes = error_fixture();
    } else if (scenario == 2) {
        put<std::uint32_t>(bytes, 0x98 + 120, 0);
        put<std::uint32_t>(bytes, 0x98 + 124, 0);
        const unsigned char fault[] = {0x31, 0xc0, 0x8b, 0, 0xc3};
        std::memcpy(bytes.data() + 0x200, fault, sizeof(fault));
    } else if (scenario != 0) {
        return 2;
    }
    const auto root = std::filesystem::temp_directory_path() / "ambrosia-trace-fixture";
    std::filesystem::create_directories(root);
    const auto path = root / "trace.exe";
    write_fixture(path, bytes);
    const auto path_text = path.string();
    const auto root_text = root.string();
    const auto result = ambrosia_native_trace(path_text.c_str(), root_text.c_str());
    std::filesystem::remove_all(root);
    return result;
}

// Metadata-only probe. The buffer is table data, never an executable mapping.
extern "C" int ambrosia_native_check_unwind(const void *data, std::size_t bytes, std::uint32_t image_bytes) {
    if (!data && bytes) {
        return 2;
    }
    try {
        const auto table =
            ambrosia::pe::UnwindTable::parse({static_cast<const std::byte *>(data), bytes}, image_bytes);
        const auto entries = table.entries();
        std::size_t samples = 0;
        if (!entries.empty()) {
            for (const auto index : {std::size_t{0}, entries.size() / 2, entries.size() - 1}) {
                const auto &entry = entries[index];
                if (entry.begin == entry.end) {
                    continue;
                }
                if (table.find(entry.begin) != &entry || table.find(entry.end - 1) != &entry ||
                    table.find(entry.end) == &entry) {
                    throw std::runtime_error("unwind table lookup sample failed");
                }
                ++samples;
            }
        }
        std::cout << "{\"unwind_records\":" << entries.size() << ",\"lookup_samples\":" << samples << "}\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "unwind metadata probe: " << error.what() << '\n';
        return 1;
    }
}

extern "C" int ambrosia_native_decode_unwind(const void *data, std::size_t bytes, std::uint32_t table_rva,
                                             std::uint32_t table_bytes) {
    if (!data || bytes > 256 * 1024 * 1024 || table_rva > bytes || table_bytes > bytes - table_rva) {
        return 2;
    }
    try {
        const std::span<const std::byte> image(static_cast<const std::byte *>(data), bytes);
        const auto table = ambrosia::pe::UnwindTable::parse(image.subspan(table_rva, table_bytes),
                                                            static_cast<std::uint32_t>(bytes));
        std::size_t decoded = 0, chained = 0, handlers = 0, max_chain = 0;
        std::map<std::string, std::size_t> unsupported;
        for (const auto &function : table.entries()) {
            try {
                const auto program = ambrosia::pe::UnwindProgram::decode(image, function);
                if (program.chained) {
                    const auto chain = ambrosia::pe::decode_unwind_chain(image, function);
                    max_chain = std::max(max_chain, chain.size());
                }
                ++decoded;
                chained += program.chained.has_value();
                handlers += program.handler_rva != 0;
            } catch (const std::runtime_error &error) {
                ++unsupported[error.what()];
            }
        }
        std::cout << "{\"records\":" << table.entries().size() << ",\"decoded\":" << decoded
                  << ",\"chained\":" << chained << ",\"handlers\":" << handlers
                  << ",\"max_chain\":" << max_chain << ",\"unsupported\":{";
        bool separator = false;
        for (const auto &[reason, count] : unsupported) {
            if (separator) {
                std::cout << ',';
            }
            std::cout << '\"' << reason << "\":" << count;
            separator = true;
        }
        std::cout << "}}\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "unwind program probe: " << error.what() << '\n';
        return 1;
    }
}

#ifdef AMBROSIA_STANDALONE_TESTS
int main() {
    return ambrosia_native_tests();
}
#endif
