#pragma once
#include "package_identity.h"
#include "readonly_files.h"
#include <vector>

namespace ambrosia::services {
struct PackageFileRequirement {
    std::string path;
    std::uint64_t bytes;
};

struct PackageInventory {
    PackageIdentity identity;
    std::vector<PackageFileRequirement> files;
};

PackageInventory parse_package_inventory(std::span<const std::byte> input);

struct InstallationReadiness {
    std::uint64_t total_bytes = 0;
    std::uint64_t installed_bytes = 0;
    std::uint32_t expected_files = 0;
    std::uint32_t ready_files = 0;

    bool completed() const {
        return expected_files && ready_files == expected_files;
    }
};

// Loose-package readiness is an exact-size check of a trusted expected inventory,
// not payload authentication. Directory references pin the confined namespace;
// each observation rechecks metadata rather than retaining open asset files.
class PackageInstallation {
  public:
    PackageInstallation(const PackageInventory &inventory, const ReadonlyFiles &files);
    InstallationReadiness inspect() const;

  private:
    struct Entry {
        ResolvedFilePath path;
        std::uint64_t bytes;
    };

    std::vector<Entry> entries_;
    std::uint64_t total_bytes_ = 0;
};
} // namespace ambrosia::services
