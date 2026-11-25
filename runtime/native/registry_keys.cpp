#include "registry_keys.h"
#include <array>
#include <stdexcept>
#include <vector>

namespace ambrosia::services {
namespace {
constexpr std::array<std::uint32_t, 5> ordinary_roots = {0, 1, 2, 3, 5};
}

RegistryKeys::RegistryKeys(std::shared_ptr<HandleIds> ids, std::size_t handle_limit)
    : ids_(std::move(ids)), handle_limit_(handle_limit) {
    if (!ids_ || handle_limit > 4096) {
        throw std::invalid_argument("invalid registry handle directory configuration");
    }
    for (const auto index : ordinary_roots) {
        auto key = std::make_shared<const Key>(index, u"");
        keys_.emplace(*key, key);
    }
}

std::uint32_t RegistryKeys::root_index(std::uintptr_t handle) {
    const auto low = static_cast<std::uint32_t>(handle);
    const auto zero_extended = static_cast<std::uintptr_t>(low);
    const auto sign_extended =
        static_cast<std::uintptr_t>(static_cast<std::intptr_t>(static_cast<std::int32_t>(low)));
    if (handle == zero_extended || handle == sign_extended) {
        for (const auto index : ordinary_roots) {
            if (low == 0x80000000u + index) {
                return index;
            }
        }
    }
    return 0xffffffffu;
}

std::u16string RegistryKeys::normalize(std::u16string_view path) {
    if (path.size() > 32767) {
        throw std::invalid_argument("registry path exceeds its runtime budget");
    }
    std::u16string result;
    result.reserve(path.size());
    std::size_t component_length = 0;
    for (auto character : path) {
        if (!character || character > 127) {
            throw std::invalid_argument("registry lookup supports ASCII case folding only");
        }
        if (character == u'\\') {
            if (!component_length) {
                throw std::invalid_argument("empty registry path component is unsupported");
            }
            component_length = 0;
        } else if (++component_length > 255) {
            throw std::invalid_argument("registry key component exceeds 255 characters");
        }
        if (character >= u'A' && character <= u'Z') {
            character += u'a' - u'A';
        }
        result.push_back(character);
    }
    if (!result.empty() && !component_length) {
        throw std::invalid_argument("trailing registry separators are unsupported");
    }
    return result;
}

std::shared_ptr<const RegistryKeys::Key> RegistryKeys::find(std::uintptr_t handle) const {
    const auto root = root_index(handle);
    if (root != 0xffffffffu) {
        return keys_.at({root, u""});
    }
    const auto found = handles_.find(handle);
    return found == handles_.end() ? nullptr : found->second;
}

std::uint32_t RegistryKeys::define_key(std::uintptr_t root, std::u16string_view path) {
    const auto index = root_index(root);
    if (index == 0xffffffffu) {
        return 6;
    }
    const auto normalized = normalize(path);
    const std::lock_guard guard(mutex_);
    std::vector<Key> additions;
    std::size_t additional_bytes = 0;
    for (std::size_t end = 0; end < normalized.size();) {
        end = normalized.find(u'\\', end);
        if (end == std::u16string::npos) {
            end = normalized.size();
        }
        Key key{index, normalized.substr(0, end)};
        if (!keys_.contains(key)) {
            additional_bytes += key.second.size() * sizeof(char16_t);
            if (keys_.size() + additions.size() >= 4096 || additional_bytes > 1024 * 1024 - path_bytes_) {
                return 8;
            }
            additions.push_back(std::move(key));
        }
        if (end < normalized.size()) {
            ++end;
        }
    }
    if (keys_.size() + additions.size() > 4096 || additional_bytes > 1024 * 1024 - path_bytes_) {
        return 8;
    }
    std::size_t inserted = 0;
    try {
        for (const auto &key : additions) {
            keys_.emplace(key, std::make_shared<const Key>(key));
            ++inserted;
        }
    } catch (...) {
        for (std::size_t index = 0; index < inserted; ++index) {
            keys_.erase(additions[index]);
        }
        throw;
    }
    path_bytes_ += additional_bytes;
    return 0;
}

KeyResult RegistryKeys::open(std::uintptr_t parent, std::u16string_view path, bool legacy) {
    const auto normalized = normalize(path);
    const std::lock_guard guard(mutex_);
    const auto base = find(parent);
    if (!base) {
        return {0, 6};
    }
    if (normalized.empty() && (legacy || root_index(parent) != 0xffffffffu)) {
        return {parent, 0};
    }
    auto name = base->second;
    if (!normalized.empty()) {
        if (name.size() + normalized.size() + (name.empty() ? 0 : 1) > 32767) {
            return {0, 206};
        }
        if (!name.empty()) {
            name += u'\\';
        }
        name += normalized;
    }
    const auto found = keys_.find({base->first, std::move(name)});
    if (found == keys_.end()) {
        return {0, 2}; // ERROR_FILE_NOT_FOUND: opening never creates a key.
    }
    if (handles_.size() >= handle_limit_) {
        return {0, 8};
    }
    const auto handle = ids_->allocate();
    if (!handle) {
        return {0, 8};
    }
    handles_.emplace(handle, found->second);
    return {handle, 0};
}

std::uint32_t RegistryKeys::close(std::uintptr_t handle) {
    const std::lock_guard guard(mutex_);
    if (root_index(handle) != 0xffffffffu) {
        return 0;
    }
    return handles_.erase(handle) ? 0 : 6;
}

bool RegistryKeys::owns(std::uintptr_t handle) const {
    const std::lock_guard guard(mutex_);
    return static_cast<bool>(find(handle));
}
} // namespace ambrosia::services
