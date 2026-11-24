#include "package_modules.h"
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <stdexcept>

namespace ambrosia::host {
namespace {
std::string ascii_key(std::string value) {
    for (auto &character : value) {
        if (character >= 'A' && character <= 'Z') {
            character += 'a' - 'A';
        }
    }
    return value;
}

bool valid_basename(const std::string &name) {
    if (name.empty() || name.size() > 255 || name == "." || name == "..") {
        return false;
    }
    for (unsigned char character : name) {
        if (character < 32 || character == 127 || character == '/' || character == '\\' || character == ':') {
            return false;
        }
    }
    return true;
}

bool regular_file(const std::filesystem::path &path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    return !error && std::filesystem::is_regular_file(status);
}
} // namespace

std::vector<std::byte> read_bootstrap_file(const std::filesystem::path &path, std::size_t limit) {
    if (!regular_file(path)) {
        throw std::runtime_error("bootstrap input is not a regular non-symlink file");
    }
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    const auto length = stream.tellg();
    if (!stream || length <= 0 || static_cast<std::uint64_t>(length) > limit) {
        throw std::runtime_error("bootstrap input is empty, unavailable or exceeds its byte budget");
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(length));
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char *>(bytes.data()), length) ||
        stream.peek() != std::char_traits<char>::eof() || !regular_file(path)) {
        throw std::runtime_error("bootstrap input changed or could not be read completely");
    }
    return bytes;
}

PackageModules::PackageModules(const std::filesystem::path &root, PackageLimits limits) : limits_(limits) {
    if (!limits_.file_bytes || !limits_.directory_entries || !limits_.modules) {
        throw std::runtime_error("package file, entry and module limits must be nonzero");
    }
    std::error_code error;
    const auto directory = std::filesystem::canonical(root, error);
    if (error || !std::filesystem::is_directory(directory, error) || error) {
        throw std::runtime_error("package root is not an available directory");
    }
    std::filesystem::directory_iterator iterator(directory, error), end;
    if (error) {
        throw std::runtime_error("cannot list package directory");
    }
    std::size_t entries = 0;
    for (; iterator != end; iterator.increment(error)) {
        if (error || ++entries > limits_.directory_entries) {
            throw std::runtime_error("package directory is unreadable or exceeds entry budget");
        }
        const auto filename = iterator->path().filename().u8string();
        const std::string name(reinterpret_cast<const char *>(filename.data()), filename.size());
        const auto key = ascii_key(name);
        if (!key.ends_with(".dll")) {
            continue;
        }
        if (!valid_basename(name) || !regular_file(iterator->path())) {
            throw std::runtime_error("package DLL must be a regular file with a valid basename");
        }
        if (paths_.size() >= limits_.modules || !paths_.emplace(key, iterator->path()).second) {
            throw std::runtime_error("package has ambiguous DLL names or exceeds module budget");
        }
    }
    if (error) {
        throw std::runtime_error("package directory enumeration failed");
    }
}

std::optional<std::vector<std::byte>> PackageModules::read(const std::string &module) {
    if (!valid_basename(module)) {
        throw std::runtime_error("package module request must be a basename");
    }
    const auto found = paths_.find(ascii_key(module));
    if (found == paths_.end()) {
        return std::nullopt;
    }
    const auto remaining = limits_.total_bytes - bytes_read_;
    auto bytes = read_bootstrap_file(found->second, std::min(limits_.file_bytes, remaining));
    bytes_read_ += bytes.size();
    ++files_read_;
    return bytes;
}
} // namespace ambrosia::host
