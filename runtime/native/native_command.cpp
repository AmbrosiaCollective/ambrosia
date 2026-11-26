#include "native_command.h"
#include "host.h"
#include "module_graph.h"
#include "package_modules.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include <charconv>
#include <limits>
#include <optional>
#include <stdexcept>

namespace ambrosia {
namespace {
struct JoinGuestThreads {
    services::NativeThreads &threads;

    ~JoinGuestThreads() {
        threads.shutdown();
    }
};

struct Options {
    std::filesystem::path image;
    std::filesystem::path package;
    std::filesystem::path work;
    bool run = false;
    bool trace = false;
    bool sparse_windows = false;
    bool boot_memory = false;
    std::optional<std::size_t> private_memory_bytes;
};

constexpr const char *usage =
    "ambrosia-host --inspect IMAGE [--package DIR] [--work DIR]\n"
    "              --run-bootstrap IMAGE [--package DIR] [--work DIR]\n"
    "              --trace-startup IMAGE [--package DIR] [--work DIR] [--sparse-windows | --boot-memory]\n"
    "                                   [--private-memory-mib N]\n"
    "Inspect maps supported PE dependencies without running callbacks.\n"
    "Run-bootstrap supports synthetic PE startup only.\n"
    "Trace-startup is diagnostic: unsupported calls/faults terminate the process.\n"
    "Sparse-windows allows 48 GiB of no-access reservations; use the approved sparse sandbox.\n"
    "Boot-memory also allows 2 GiB private reservations; requires its separately approved memory sandbox.\n"
    "Private-memory-mib selects 1..8192 MiB for diagnostics; use a matching approved sandbox.\n";

std::size_t parse_private_memory(const std::filesystem::path &argument) {
    const auto text = argument.string();
    std::uint64_t mib = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), mib);
    constexpr std::size_t one_mib = 1024 * 1024;
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !mib || mib > 8192 ||
        mib > std::numeric_limits<std::size_t>::max() / one_mib) {
        throw std::invalid_argument("private-memory-mib must be a decimal integer from 1 to 8192");
    }
    return static_cast<std::size_t>(mib) * one_mib;
}

Options parse_options(std::span<const std::filesystem::path> arguments) {
    Options options;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto &flag = arguments[index];
        if (flag == "--private-memory-mib") {
            if (options.private_memory_bytes) {
                throw std::invalid_argument("duplicate private-memory-mib option");
            }
            if (++index == arguments.size()) {
                throw std::invalid_argument("private-memory-mib requires a value");
            }
            options.private_memory_bytes = parse_private_memory(arguments[index]);
            continue;
        }
        if (flag == "--sparse-windows" || flag == "--boot-memory") {
            if (options.sparse_windows || options.boot_memory) {
                throw std::invalid_argument("duplicate sparse-window or boot-memory option");
            }
            options.sparse_windows = flag == "--sparse-windows";
            options.boot_memory = flag == "--boot-memory";
            continue;
        }
        if (flag != "--inspect" && flag != "--run-bootstrap" && flag != "--trace-startup" &&
            flag != "--package" && flag != "--work") {
            throw std::invalid_argument("unknown bootstrap option");
        }
        if (++index == arguments.size() || arguments[index].empty() ||
            arguments[index].native().starts_with(std::filesystem::path("--").native())) {
            throw std::invalid_argument("bootstrap option requires a path");
        }
        const auto &value = arguments[index];
        if (flag == "--inspect" || flag == "--run-bootstrap" || flag == "--trace-startup") {
            if (!options.image.empty()) {
                throw std::invalid_argument("choose one image and bootstrap mode");
            }
            options.image = value;
            options.trace = flag == "--trace-startup";
            options.run = flag != "--inspect";
        } else if (flag == "--package") {
            if (!options.package.empty()) {
                throw std::invalid_argument("duplicate package option");
            }
            options.package = value;
        } else {
            if (!options.work.empty()) {
                throw std::invalid_argument("duplicate work option");
            }
            options.work = value;
        }
    }
    if (options.image.empty()) {
        throw std::invalid_argument("choose an image and bootstrap mode");
    }
    if ((options.sparse_windows || options.boot_memory) && !options.trace) {
        throw std::invalid_argument("sparse-window option requires trace-startup mode");
    }
    if (options.private_memory_bytes && !options.trace) {
        throw std::invalid_argument("private-memory-mib requires trace-startup mode");
    }
    if (options.private_memory_bytes && options.boot_memory) {
        throw std::invalid_argument("private-memory-mib conflicts with the fixed boot-memory profile");
    }
    return options;
}
} // namespace

int run_native_command(std::span<const std::filesystem::path> arguments, std::ostream &output,
                       std::ostream &errors) {
    if (arguments.size() == 1 && arguments.front() == "--help") {
        output << usage;
        return 0;
    }
    try {
        const auto options = parse_options(arguments);
        const auto paths = host::resolve_paths(options.image, options.package, options.work);
        pe::ModuleLimits module_limits;
        if (!options.run || options.trace) {
            module_limits.image.mode =
                options.trace ? pe::LoadMode::diagnostic_startup : pe::LoadMode::inspect;
            module_limits.image.file_bytes = 128 * 1024 * 1024;
            module_limits.image.mapped_bytes = 256 * 1024 * 1024;
            module_limits.source_bytes = 256 * 1024 * 1024;
            module_limits.mapped_bytes = 384 * 1024 * 1024;
        }
        auto bytes = host::read_bootstrap_file(paths.image, module_limits.image.file_bytes);
        const auto root_bytes = bytes.size();
        host::PackageLimits package_limits;
        package_limits.total_bytes = module_limits.source_bytes - root_bytes;
        host::PackageModules package(paths.package, package_limits);
        services::MemoryLimits memory_limits;
        if (options.boot_memory) {
            memory_limits.private_bytes = 2ull << 30;
        }
        if (options.private_memory_bytes) {
            memory_limits.private_bytes = *options.private_memory_bytes;
        }
        if (options.sparse_windows || options.boot_memory) {
            memory_limits.window_bytes = 48ull << 30;
        }
        if (options.private_memory_bytes || options.sparse_windows || options.boot_memory) {
            memory_limits.total_bytes = memory_limits.window_bytes + memory_limits.private_bytes;
        }
        services::Runtime service_runtime(memory_limits);
        service_runtime.startup().bind_image(paths.image);
        std::unique_ptr<diagnostics::ImportStops> stops;
        pe::Resolver diagnostic_stop;
        if (options.trace) {
            stops = std::make_unique<diagnostics::ImportStops>();
            diagnostic_stop = [&](const auto &symbol) { return stops->resolve(symbol); };
        }
        pe::ModuleGraph graph([&](const auto &symbol) { return service_runtime.resolve(symbol); },
                              [&](const auto &name) { return package.read(name); }, module_limits,
                              diagnostic_stop, [&](const auto &name) { return package.source_path(name); });
        const auto filename = paths.image.filename().u8string();
        const std::string root(reinterpret_cast<const char *>(filename.data()), filename.size());
        graph.add(root, bytes, paths.image);
        // Registration owns a copy. Drop the file buffer before mapping large images.
        std::vector<std::byte>().swap(bytes);
        graph.load(root);
        const auto statistics = graph.statistics();
        std::optional<std::uint64_t> result;
        if (options.run) {
            if (!statistics.executable_root) {
                throw std::runtime_error("bootstrap mode requires an executable root");
            }
            services::Scope scope(service_runtime, &graph);
            if (options.trace) {
                stops->seal();
                diagnostics::ImportStopScope import_scope(*stops);
                diagnostics::FaultScope fault_scope(graph.diagnostic_ranges());
                // Diagnostics deliberately stop before any unsupported service
                // can return a fabricated result. Faults are fatal, not handled.
                errors << "TRACE armed modules=" << statistics.modules << " stop_imports=" << stops->size()
                       << '\n'
                       << std::flush;
                errors << "TRACE stage=module-initialization\n" << std::flush;
                auto execution = graph.start();
                JoinGuestThreads join_workers{service_runtime.threads()};
                errors << "TRACE stage=executable-entry\n" << std::flush;
                const auto returned = execution.run_bootstrap();
                service_runtime.threads().shutdown();
                errors << "TRACE stage=returned-entry-cleanup\n" << std::flush;
                execution.with_guest_context([&] { service_runtime.exit_callbacks().execute(); });
                output << "{\"diagnostic_startup\":true,\"trace_returned\":" << returned << "}\n";
                return 3;
            }
            auto execution = graph.start();
            JoinGuestThreads join_workers{service_runtime.threads()};
            result = execution.run_bootstrap();
            service_runtime.threads().shutdown();
            execution.with_guest_context([&] { service_runtime.exit_callbacks().execute(); });
        }
        output << "{\"modules\":" << statistics.modules << ",\"mapped_bytes\":" << statistics.mapped_bytes
               << ",\"imports\":" << statistics.imports << ",\"files_read\":" << package.files_read() + 1
               << ",\"bytes_read\":" << package.bytes_read() + root_bytes;
        if (statistics.inspection_only) {
            output << ",\"inspection_only\":true,\"unresolved_imports\":" << statistics.unresolved_imports
                   << ",\"unwind_records\":" << statistics.unwind_records
                   << ",\"unsupported_directories\":" << statistics.unsupported_directories;
        }
        if (result) {
            output << ",\"bootstrap_result\":" << *result;
        }
        output << "}\n";
        return 0;
    } catch (const std::invalid_argument &error) {
        errors << "ambrosia-host: " << error.what() << '\n' << usage;
        return 2;
    } catch (const std::filesystem::filesystem_error &) {
        errors << "ambrosia-host: filesystem operation failed\n";
        return 1;
    } catch (const std::exception &error) {
        errors << "ambrosia-host: " << error.what() << '\n';
        return 1;
    }
}
} // namespace ambrosia
