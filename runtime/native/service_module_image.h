#pragma once
#include "host.h"
#include "pe_exports.h"
#include <memory>
#include <vector>

namespace ambrosia::services {
struct ServiceExport {
    std::string name;
    std::uintptr_t target = 0;
    std::string forwarder;
    std::uint16_t ordinal = 0; // Zero selects an unused private image ordinal.
};

// An original in-memory PE image describing this runtime's native adapters.
// It is not a copied system DLL and has no entrypoint, imports or TLS callbacks.
class ServiceModuleImage {
  public:
    ServiceModuleImage(std::string module, std::vector<ServiceExport> exports);

    std::span<const std::byte> bytes() const {
        return {memory_->data(), memory_->size()};
    }

    std::uintptr_t base() const {
        return reinterpret_cast<std::uintptr_t>(memory_->data());
    }

    const pe::ExportTable &exports() const {
        return exports_;
    }

    std::uintptr_t direct_export(const std::string &name) const;

  private:
    std::unique_ptr<host::Mapping> memory_;
    pe::ExportTable exports_;
};
} // namespace ambrosia::services
