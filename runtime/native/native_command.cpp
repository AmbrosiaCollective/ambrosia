#include "native_command.h"
#include "host.h"
#include "module_graph.h"
#include "package_modules.h"
#include <optional>
#include <stdexcept>

namespace ambrosia {
namespace {
struct Options {
    std::filesystem::path image;
    std::filesystem::path package;
    std::filesystem::path work;
    bool run = false;
};

constexpr const char *usage =
    "ambrosia-host --inspect IMAGE [--package DIR] [--work DIR]\n"
    "              --run-bootstrap IMAGE [--package DIR] [--work DIR]\n"
    "Synthetic PE bootstrap only. Inspect maps dependencies without running callbacks.\n";

Options parse_options(std::span<const std::filesystem::path> arguments) {
    Options options;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto &flag = arguments[index];
        if (flag != "--inspect" && flag != "--run-bootstrap" && flag != "--package" && flag != "--work") {
            throw std::invalid_argument("unknown bootstrap option");
        }
        if (++index == arguments.size() || arguments[index].empty() ||
            arguments[index].native().starts_with(std::filesystem::path("--").native())) {
            throw std::invalid_argument("bootstrap option requires a path");
        }
        const auto &value = arguments[index];
        if (flag == "--inspect" || flag == "--run-bootstrap") {
            if (!options.image.empty()) {
                throw std::invalid_argument("choose one image and bootstrap mode");
            }
            options.image = value;
            options.run = flag == "--run-bootstrap";
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
        constexpr std::size_t source_limit = 64 * 1024 * 1024;
        auto bytes = host::read_bootstrap_file(paths.image, source_limit);
        host::PackageLimits limits;
        limits.total_bytes = source_limit - bytes.size();
        host::PackageModules package(paths.package, limits);
        pe::ModuleGraph graph({}, [&](const auto &name) { return package.read(name); });
        const auto filename = paths.image.filename().u8string();
        const std::string root(reinterpret_cast<const char *>(filename.data()), filename.size());
        graph.add(root, bytes);
        graph.load(root);
        const auto statistics = graph.statistics();
        std::optional<std::uint64_t> result;
        if (options.run) {
            if (!statistics.executable_root) {
                throw std::runtime_error("bootstrap mode requires an executable root");
            }
            auto execution = graph.start();
            result = execution.run_bootstrap();
        }
        output << "{\"modules\":" << statistics.modules << ",\"mapped_bytes\":" << statistics.mapped_bytes
               << ",\"imports\":" << statistics.imports << ",\"files_read\":" << package.files_read() + 1
               << ",\"bytes_read\":" << package.bytes_read() + bytes.size();
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
