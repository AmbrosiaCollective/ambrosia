#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace ambrosia::services {
struct PackageIdentity {
    std::string name;
    std::string publisher;
    std::array<std::uint16_t, 4> version{};
    bool operator==(const PackageIdentity &) const = default;
};

enum class PackageDescriptor { game_config, appx_manifest };
inline constexpr std::size_t package_descriptor_limit = 512 * 1024;
inline constexpr std::size_t package_identifier_limit = 32;

PackageIdentity parse_package_identity(std::span<const std::byte> input, PackageDescriptor kind);
} // namespace ambrosia::services
