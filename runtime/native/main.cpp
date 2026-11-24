#include "pe_image.h"
#include <fstream>
#include <iostream>
#include <vector>

namespace {
template<class Character> int command(int argc, Character** argv) {
    try {
        std::filesystem::path image_path, package, work;
        bool run = false;
        for (int i = 1; i < argc; ++i) {
            const std::filesystem::path argument(argv[i]);
            if (argument == "--help") {
                std::cout << "ambrosia-host --inspect IMAGE [--package DIR] [--work DIR]\n"
                             "              --run-bootstrap IMAGE [--package DIR] [--work DIR]\n"
                             "Synthetic PE bootstrap only. No retail TLS, services or graphics yet.\n";
                return 0;
            }
            if (i + 1 >= argc) throw std::runtime_error("option requires a path");
            if (argument == "--inspect" || argument == "--run-bootstrap") {
                if (!image_path.empty()) throw std::runtime_error("choose one image and mode");
                run = argument == "--run-bootstrap";
                image_path = std::filesystem::path(argv[++i]);
            } else if (argument == "--package") {
                if (!package.empty()) throw std::runtime_error("duplicate package option");
                package = std::filesystem::path(argv[++i]);
            } else if (argument == "--work") {
                if (!work.empty()) throw std::runtime_error("duplicate work option");
                work = std::filesystem::path(argv[++i]);
            } else throw std::runtime_error("unknown option");
        }
        const auto paths = ambrosia::host::resolve_paths(image_path, package, work);
        // Bound allocation before reading, and require an exact read. Nothing
        // writes to the package or creates work directories during bootstrap.
        std::ifstream stream(paths.image, std::ios::binary | std::ios::ate);
        const auto length = stream.tellg();
        if (!stream || length <= 0 || length > 64 * 1024 * 1024)
            throw std::runtime_error("bootstrap image size unavailable or exceeds 64 MiB");
        std::vector<std::byte> bytes(static_cast<std::size_t>(length));
        stream.seekg(0);
        if (!stream.read(reinterpret_cast<char*>(bytes.data()), length))
            throw std::runtime_error("cannot read complete bootstrap image");
        // There is intentionally no host-DLL fallback or zero-return stub.
        auto image = ambrosia::pe::Image::load(bytes, {});
        std::cout << "{\"mapped_bytes\":" << image.size() << ",\"imports\":" << image.imports().size();
        if (run) std::cout << ",\"bootstrap_result\":" << image.run_bootstrap();
        std::cout << "}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ambrosia-host: " << error.what() << '\n';
        return 1;
    }
}
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) { return command(argc, argv); }
#else
int main(int argc, char** argv) { return command(argc, argv); }
#endif
