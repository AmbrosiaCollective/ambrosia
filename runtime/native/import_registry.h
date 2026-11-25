#pragma once
#include "pe_image.h"
#include <map>

namespace ambrosia::services {
class ImportRegistry {
  public:
    using Entries = std::map<std::pair<std::string, std::string>, std::uintptr_t>;
    static std::string module_name(std::string name);
    void add(std::string module, std::string name, std::uintptr_t function);
    std::uintptr_t resolve(const pe::Import &symbol) const;
    bool has_module(std::string name) const;

    const Entries &entries() const noexcept {
        return functions_;
    }

  private:
    Entries functions_;
};
} // namespace ambrosia::services
