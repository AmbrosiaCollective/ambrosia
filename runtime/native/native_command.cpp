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
struct JoinGuestWorkers {
    services::Runtime &runtime;

    ~JoinGuestWorkers() {
        runtime.shutdown_workers();
    }
};

struct Options {
    std::filesystem::path image;
    std::filesystem::path package;
    std::filesystem::path work;
    std::filesystem::path inventory;
    std::filesystem::path storage;
    std::filesystem::path presentation;
    std::optional<std::string> locale_name;
    std::optional<int> physical_backing_descriptor;
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
    "              Run modes also accept --package-inventory FILE (trusted expected file metadata).\n"
    "              --storage-root DIR selects an existing separate writable state directory.\n"
    "              --locale-name TAG selects the guest formatting locale (default en-US).\n"
    "              --presentation-root DIR selects explicit trusted host-presentation endpoints.\n"
    "              --physical-backing-fd N transfers a reviewed sealed backing descriptor in run modes.\n"
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

int parse_backing_descriptor(const std::filesystem::path &argument) {
    const auto text = argument.string();
    int descriptor = -1;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), descriptor);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || descriptor < 3) {
        throw std::invalid_argument("physical-backing-fd requires an owned decimal descriptor at least 3");
    }
    return descriptor;
}

Options parse_options(std::span<const std::filesystem::path> arguments) {
    Options options;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto &flag = arguments[index];
        if (flag == "--physical-backing-fd") {
            if (options.physical_backing_descriptor) {
                throw std::invalid_argument("duplicate physical-backing-fd option");
            }
            if (++index == arguments.size()) {
                throw std::invalid_argument("physical-backing-fd requires a value");
            }
            options.physical_backing_descriptor = parse_backing_descriptor(arguments[index]);
            continue;
        }
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
            flag != "--package" && flag != "--work" && flag != "--package-inventory" &&
            flag != "--storage-root" && flag != "--locale-name" && flag != "--presentation-root") {
            throw std::invalid_argument("unknown bootstrap option");
        }
        if (++index == arguments.size() || arguments[index].empty() ||
            arguments[index].native().starts_with(std::filesystem::path("--").native())) {
            throw std::invalid_argument("bootstrap option requires a value");
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
        } else if (flag == "--locale-name") {
            if (options.locale_name) {
                throw std::invalid_argument("duplicate locale-name option");
            }
            const auto utf8 = value.u8string();
            options.locale_name = std::string(utf8.begin(), utf8.end());
        } else if (flag == "--presentation-root") {
            if (!options.presentation.empty()) {
                throw std::invalid_argument("duplicate presentation-root option");
            }
            options.presentation = value;
        } else if (flag == "--storage-root") {
            if (!options.storage.empty()) {
                throw std::invalid_argument("duplicate storage root option");
            }
            options.storage = value;
        } else if (flag == "--package-inventory") {
            if (!options.inventory.empty()) {
                throw std::invalid_argument("duplicate package inventory option");
            }
            options.inventory = value;
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
    if (options.locale_name && !options.run) {
        throw std::invalid_argument("locale-name requires a run mode");
    }
    if (!options.presentation.empty() && !options.run) {
        throw std::invalid_argument("presentation-root requires a run mode");
    }
    if (options.physical_backing_descriptor && !options.run) {
        throw std::invalid_argument("physical-backing-fd requires a run mode");
    }
    if (!options.storage.empty() && !options.run) {
        throw std::invalid_argument("storage root requires a run mode");
    }
    if (!options.inventory.empty() && !options.run) {
        throw std::invalid_argument("package inventory requires a run mode");
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
        if (!options.presentation.empty()) {
            service_runtime.windows().bind(options.presentation);
        }
        if (options.physical_backing_descriptor) {
            service_runtime.physical_pages().bind(host::SharedBacking::adopt_descriptor(
                *options.physical_backing_descriptor, services::PhysicalPages::maximum_backing_bytes));
        }
        if (options.locale_name) {
            service_runtime.user_locale().configure(*options.locale_name);
        }
        service_runtime.startup().bind_image(paths.image);
        if (options.run) {
            service_runtime.files().bind(paths.package, paths.image);
            service_runtime.gdk_display().select_sdr_output();
            if (!options.storage.empty()) {
                // Validate separation before the host's writable-directory probe.
                service_runtime.files().bind_additional_native_root(options.storage);
                service_runtime.persistent_storage().bind(options.storage);
            }
            service_runtime.gdk_packages().bind(service_runtime.files());
            if (!options.inventory.empty()) {
                const auto inventory =
                    host::read_bootstrap_file(options.inventory, services::package_descriptor_limit);
                service_runtime.gdk_packages().bind_installation(inventory, service_runtime.files());
            }
        }
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
            if (service_runtime.gdk_packages().installation()) {
                service_runtime.gdk_packages().bind_current_game_catalog();
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
                JoinGuestWorkers join_workers{service_runtime};
                errors << "TRACE stage=executable-entry\n" << std::flush;
                const auto returned = execution.run_bootstrap();
                service_runtime.threads().owner_returned(static_cast<std::uint32_t>(returned));
                service_runtime.shutdown_workers();
                errors << "TRACE stage=returned-entry-cleanup\n" << std::flush;
                execution.with_guest_context([&] { service_runtime.exit_callbacks().execute(); });
                output << "{\"diagnostic_startup\":true,\"trace_returned\":" << returned << "}\n";
                return 3;
            }
            auto execution = graph.start();
            JoinGuestWorkers join_workers{service_runtime};
            result = execution.run_bootstrap();
            service_runtime.threads().owner_returned(static_cast<std::uint32_t>(*result));
            service_runtime.shutdown_workers();
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
