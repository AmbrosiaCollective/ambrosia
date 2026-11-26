#pragma once
#include "pe_image.h"
#include <map>

namespace ambrosia::services {
class ImportRegistry {
  public:
    using Entries = std::map<std::pair<std::string, std::string>, std::uintptr_t>;
    using Ordinals = std::map<std::pair<std::string, std::uint16_t>, std::string>;
    void add_ordinal(std::string module, std::uint16_t ordinal, std::string name);
    static std::string module_name(std::string name);
    void add(std::string module, std::string name, std::uintptr_t function);
    std::uintptr_t resolve(const pe::Import &symbol) const;
    bool has_module(std::string name) const;

    const Entries &entries() const noexcept {
        return functions_;
    }

    const Ordinals &ordinals() const noexcept {
        return ordinals_;
    }

  private:
    Entries functions_;
    Ordinals ordinals_;
};
} // namespace ambrosia::services
