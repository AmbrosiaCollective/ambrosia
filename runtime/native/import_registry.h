#pragma once
#include "pe_image.h"
#include <map>

namespace ambrosia::services {
class ImportRegistry {
  public:
    void add(std::string module, std::string name, std::uintptr_t function);
    std::uintptr_t resolve(const pe::Import &symbol) const;

  private:
    std::map<std::pair<std::string, std::string>, std::uintptr_t> functions_;
};
} // namespace ambrosia::services
