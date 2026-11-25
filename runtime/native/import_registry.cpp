#include "import_registry.h"
#include <stdexcept>

namespace ambrosia::services {
namespace {
std::string module_key(std::string name) {
    if (name.empty()) {
        throw std::runtime_error("service module cannot be empty");
    }
    for (auto &character : name) {
        const auto value = static_cast<unsigned char>(character);
        if (value < 32 || value == 127 || character == '/' || character == '\\' || character == ':') {
            throw std::runtime_error("service module must be a basename");
        }
        if (character >= 'A' && character <= 'Z') {
            character += 'a' - 'A';
        }
    }
    return name;
}
} // namespace

void ImportRegistry::add(std::string module, std::string name, std::uintptr_t function) {
    if (name.empty() || !function || functions_.size() >= 4096) {
        throw std::runtime_error("invalid service registration or registry limit");
    }
    if (!functions_.emplace(std::make_pair(module_key(std::move(module)), std::move(name)), function)
             .second) {
        throw std::runtime_error("duplicate service registration");
    }
}

std::string ImportRegistry::module_name(std::string name) {
    if (!name.empty() && name.back() == '.') {
        name.pop_back();
    } else if (name.find('.') == std::string::npos) {
        name += ".dll";
    }
    return module_key(std::move(name));
}

bool ImportRegistry::has_module(std::string name) const {
    const auto key = module_name(std::move(name));
    const auto found = functions_.lower_bound({key, ""});
    return found != functions_.end() && found->first.first == key;
}

std::uintptr_t ImportRegistry::resolve(const pe::Import &symbol) const {
    if (symbol.by_ordinal) {
        return 0;
    }
    auto found = functions_.find({module_key(symbol.module), symbol.name});
    return found == functions_.end() ? 0 : found->second;
}
} // namespace ambrosia::services
