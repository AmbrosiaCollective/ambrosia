#pragma once
#include "import_registry.h"
#include "service_module_image.h"
#include <mutex>
#include <optional>

namespace ambrosia::services {
class NativeModules {
  public:
    explicit NativeModules(const ImportRegistry &registry);
    bool has_module(std::string name) const;
    std::uintptr_t handle(std::string name);
    std::uintptr_t entrypoint(std::uintptr_t native_target) const;
    std::optional<std::uintptr_t> export_address(std::uintptr_t handle, const std::string &name) const;
    std::uintptr_t module_base_for_address(std::uintptr_t address) const;

  private:
    std::unique_ptr<ServiceModuleImage> canonical_;
    std::map<std::uintptr_t, std::string> target_names_;
    std::map<std::uintptr_t, std::uintptr_t> entrypoints_;
    std::map<std::string, std::map<std::string, std::uintptr_t>> exports_;
    mutable std::mutex mutex_;
    std::map<std::string, std::unique_ptr<ServiceModuleImage>> facades_;
    std::size_t mapped_bytes_ = 0;
};
} // namespace ambrosia::services
