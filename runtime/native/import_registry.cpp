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
    if (name.empty() || !function || functions_.size() + ordinals_.size() >= 4096) {
        throw std::runtime_error("invalid service registration or registry limit");
    }
    if (!functions_.emplace(std::make_pair(module_key(std::move(module)), std::move(name)), function)
             .second) {
        throw std::runtime_error("duplicate service registration");
    }
}

void ImportRegistry::add_ordinal(std::string module, std::uint16_t ordinal, std::string name) {
    module = module_key(std::move(module));
    if (!ordinal || functions_.size() + ordinals_.size() >= 4096 || !functions_.contains({module, name})) {
        throw std::runtime_error("ordinal alias requires a registered named target and available capacity");
    }
    for (const auto &[identity, existing] : ordinals_) {
        if (identity.first == module && existing == name) {
            throw std::runtime_error("named service already has a public ordinal");
        }
    }
    if (!ordinals_.emplace(std::make_pair(std::move(module), ordinal), std::move(name)).second) {
        throw std::runtime_error("duplicate native service ordinal");
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
    const auto module = module_key(symbol.module);
    std::string name = symbol.name;
    if (symbol.by_ordinal) {
        const auto alias = ordinals_.find({module, symbol.ordinal});
        if (alias == ordinals_.end()) {
            return 0;
        }
        name = alias->second;
    }
    auto found = functions_.find({module, name});
    return found == functions_.end() ? 0 : found->second;
}
} // namespace ambrosia::services
