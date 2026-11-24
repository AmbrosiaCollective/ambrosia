#include "native_command.h"
#include "host.h"
#include "module_graph.h"
#include "package_modules.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include <optional>
#include <stdexcept>

namespace ambrosia {
namespace {
struct Options {
    std::filesystem::path image;
    std::filesystem::path package;
    std::filesystem::path work;
    bool run = false;
    bool trace = false;
};

constexpr const char *usage =
    "ambrosia-host --inspect IMAGE [--package DIR] [--work DIR]\n"
    "              --run-bootstrap IMAGE [--package DIR] [--work DIR]\n"
    "              --trace-startup IMAGE [--package DIR] [--work DIR]\n"
    "Inspect maps supported PE dependencies without running callbacks.\n"
    "Run-bootstrap supports synthetic PE startup only.\n"
    "Trace-startup is diagnostic: unsupported calls/faults terminate the process.\n";

Options parse_options(std::span<const std::filesystem::path> arguments) {
    Options options;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto &flag = arguments[index];
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
        services::Runtime service_runtime;
        std::unique_ptr<diagnostics::ImportStops> stops;
        pe::Resolver diagnostic_stop;
        if (options.trace) {
            stops = std::make_unique<diagnostics::ImportStops>();
            diagnostic_stop = [&](const auto &symbol) { return stops->resolve(symbol); };
        }
        pe::ModuleGraph graph([&](const auto &symbol) { return service_runtime.resolve(symbol); },
                              [&](const auto &name) { return package.read(name); }, module_limits,
                              diagnostic_stop);
        const auto filename = paths.image.filename().u8string();
        const std::string root(reinterpret_cast<const char *>(filename.data()), filename.size());
        graph.add(root, bytes);
        // Registration owns a copy. Drop the file buffer before mapping large images.
        std::vector<std::byte>().swap(bytes);
        graph.load(root);
        const auto statistics = graph.statistics();
        std::optional<std::uint64_t> result;
        if (options.run) {
            if (!statistics.executable_root) {
                throw std::runtime_error("bootstrap mode requires an executable root");
            }
            services::Scope scope(service_runtime);
            if (options.trace) {
                stops->seal();
                diagnostics::ImportStopScope import_scope(*stops);
                diagnostics::FaultScope fault_scope(graph.diagnostic_ranges());
                // Diagnostics deliberately stop before any unsupported service
                // can return a fabricated result. Faults are fatal, not handled.
                errors << "TRACE armed modules=" << statistics.modules << " stop_imports=" << stops->size()
                       << '\n'
                       << std::flush;
                auto execution = graph.start();
                const auto returned = execution.run_bootstrap();
                output << "{\"diagnostic_startup\":true,\"trace_returned\":" << returned << "}\n";
                return 3;
            }
            auto execution = graph.start();
            result = execution.run_bootstrap();
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
