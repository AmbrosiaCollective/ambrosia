#pragma once
#include "host_graphics.h"

void graphics_transport_rejection_checks() {
#ifdef __linux__
    const auto root = std::filesystem::temp_directory_path() / "ambrosia-graphics-rejection";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto rejects = [&](const char *description) {
        bool rejected = false;
        try {
            ambrosia::host::Graphics graphics(root);
        } catch (const std::runtime_error &) {
            rejected = true;
        }
        check(rejected, description);
    };
    const auto write = [&](const char *name, std::size_t size, char value = 0) {
        std::ofstream stream(root / name, std::ios::binary | std::ios::trunc);
        const std::string bytes(size, value);
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!stream) {
            throw std::runtime_error("cannot prepare synthetic graphics endpoint");
        }
    };
    rejects("native graphics rejects missing endpoints");
    write("request", 63);
    rejects("native graphics rejects a truncated request");
    write("request", 64);
    write("response", 63);
    rejects("native graphics rejects a truncated response");
    write("response", 64);
    std::filesystem::create_hard_link(root / "request", root / "alias");
    rejects("native graphics rejects multiply linked endpoints");
    std::filesystem::remove(root / "alias");
    std::filesystem::remove(root / "request");
    std::filesystem::create_symlink(root / "response", root / "request");
    rejects("native graphics rejects symlinked endpoints");
    std::filesystem::remove(root / "request");
    write("request", 64);
    write("response", 64, 'x');
    rejects("native graphics requires a fresh zeroed service response");
    std::filesystem::remove_all(root);
#endif
}

void graphics_transport_live_checks(const std::filesystem::path &root) {
    ambrosia::host::Graphics graphics(root);
    const auto first = graphics.create_device();
    check(first.id != 0 && first.api_version >= ((1u << 22) | (3u << 12)),
          "native graphics creates a real host-owned Vulkan device");
    const auto queried = graphics.query_device(first.id);
    check(queried.id == first.id && queried.api_version == first.api_version,
          "native graphics reads back owned device capabilities");
    bool rejected = false;
    try {
        (void)graphics.create_device();
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    check(rejected, "native graphics rejects duplicate live creation locally");
    rejected = false;
    try {
        (void)graphics.query_device(first.id + 100);
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    check(rejected, "native graphics rejects foreign ownership locally");
    graphics.destroy_device(first.id);
    rejected = false;
    try {
        (void)graphics.query_device(first.id);
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    check(rejected, "native graphics rejects a released ownership ID locally");
    const auto second = graphics.create_device();
    check(second.id > first.id && second.api_version == first.api_version,
          "native graphics recreation retains capabilities without reusing IDs");
    const auto final = graphics.query_device(second.id);
    check(final.id == second.id, "native graphics reads back the recreated device");
    // Leave the second real device live. Session destruction must ask the
    // service to close it before the isolated client returns.
}
