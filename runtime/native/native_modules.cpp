#include "native_modules.h"
#include <stdexcept>

namespace ambrosia::services {
namespace {
constexpr const char *canonical_name = "ambrosia-native.dll";
constexpr std::size_t module_limit = 64;
constexpr std::size_t mapping_limit = 16 * 1024 * 1024;

bool contains(const ServiceModuleImage &image, std::uintptr_t address) {
    return address >= image.base() && address - image.base() < image.bytes().size();
}
} // namespace

NativeModules::NativeModules(const ImportRegistry &registry) {
    for (const auto &[identity, target] : registry.entries()) {
        exports_[identity.first].emplace(identity.second, target);
        target_names_.try_emplace(target);
    }
    for (const auto &[identity, name] : registry.ordinals()) {
        const auto target = exports_.at(identity.first).at(name);
        ordinal_targets_.emplace(identity, target);
        named_ordinals_.emplace(std::make_pair(identity.first, name), identity.second);
    }
    if (exports_.contains(canonical_name) || exports_.size() + 1 > module_limit) {
        throw std::runtime_error("native service module catalog is reserved or exceeds its budget");
    }
    std::vector<ServiceExport> canonical_exports;
    unsigned index = 0;
    for (auto &[target, name] : target_names_) {
        name = "entry" + std::to_string(++index);
        canonical_exports.push_back({name, target, {}});
    }
    canonical_ = std::make_unique<ServiceModuleImage>(canonical_name, std::move(canonical_exports));
    mapped_bytes_ = canonical_->bytes().size();
    for (const auto &[target, name] : target_names_) {
        entrypoints_.emplace(target, canonical_->direct_export(name));
    }
}

bool NativeModules::has_module(std::string name) const {
    const auto key = ImportRegistry::module_name(std::move(name));
    return key == canonical_name || exports_.contains(key);
}

std::uintptr_t NativeModules::handle(std::string name) {
    const auto key = ImportRegistry::module_name(std::move(name));
    if (key == canonical_name) {
        return canonical_->base();
    }
    const auto symbols = exports_.find(key);
    if (symbols == exports_.end()) {
        return 0;
    }
    const std::lock_guard guard(mutex_);
    if (const auto existing = facades_.find(key); existing != facades_.end()) {
        return existing->second->base();
    }
    // Each image is independently capped at 512 KiB. Reserve that worst case
    // against the aggregate before any new executable mapping is allocated.
    if (facades_.size() + 1 >= module_limit || mapped_bytes_ > mapping_limit - 512 * 1024) {
        throw std::runtime_error("native service facade mapping budget exhausted");
    }
    std::vector<ServiceExport> forwarded;
    for (const auto &[symbol, target] : symbols->second) {
        // A single canonical stub per unique native target preserves pointer
        // identity across static imports, dynamic lookups and module aliases.
        const auto ordinal = named_ordinals_.find({key, symbol});
        const auto value = ordinal == named_ordinals_.end() ? std::uint16_t{0} : ordinal->second;
        forwarded.push_back({symbol, 0, "ambrosia-native." + target_names_.at(target), value});
    }
    auto image = std::make_unique<ServiceModuleImage>(key, std::move(forwarded));
    const auto bytes = image->bytes().size();
    const auto base = image->base();
    facades_.emplace(key, std::move(image));
    mapped_bytes_ += bytes;
    return base;
}

std::uintptr_t NativeModules::entrypoint(std::uintptr_t native_target) const {
    const auto found = entrypoints_.find(native_target);
    if (found == entrypoints_.end()) {
        throw std::runtime_error("native target has no canonical service entrypoint");
    }
    return found->second;
}

std::optional<std::uintptr_t> NativeModules::export_address(std::uintptr_t handle,
                                                            const std::string &name) const {
    if (handle == canonical_->base()) {
        return canonical_->direct_export(name);
    }
    const std::lock_guard guard(mutex_);
    for (const auto &[module, image] : facades_) {
        if (image->base() == handle) {
            const auto &symbols = exports_.at(module);
            const auto found = symbols.find(name);
            return found == symbols.end() ? 0 : entrypoints_.at(found->second);
        }
    }
    return std::nullopt;
}

std::optional<std::uintptr_t> NativeModules::export_address(std::uintptr_t handle,
                                                            std::uint16_t ordinal) const {
    if (handle == canonical_->base()) {
        const auto found = canonical_->exports().ordinals.find(ordinal);
        return found == canonical_->exports().ordinals.end() ? 0 : canonical_->base() + found->second.rva;
    }
    const std::lock_guard guard(mutex_);
    for (const auto &[module, image] : facades_) {
        if (image->base() == handle) {
            const auto found = ordinal_targets_.find({module, ordinal});
            return found == ordinal_targets_.end() ? 0 : entrypoints_.at(found->second);
        }
    }
    return std::nullopt;
}

std::uintptr_t NativeModules::module_base_for_address(std::uintptr_t address) const {
    if (contains(*canonical_, address)) {
        return canonical_->base();
    }
    const std::lock_guard guard(mutex_);
    for (const auto &[name, image] : facades_) {
        (void)name;
        if (contains(*image, address)) {
            return image->base();
        }
    }
    return 0;
}
} // namespace ambrosia::services
