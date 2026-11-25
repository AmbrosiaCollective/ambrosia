#include "library_references.h"

namespace ambrosia::services {
bool LibraryReferences::retain(std::uintptr_t module) {
    const std::lock_guard guard(mutex_);
    if (!module || total_ == 4096 || (!references_.contains(module) && references_.size() == 128)) {
        return false;
    }
    ++references_[module];
    ++total_;
    return true;
}

bool LibraryReferences::release(std::uintptr_t module) {
    const std::lock_guard guard(mutex_);
    const auto found = references_.find(module);
    if (found == references_.end()) {
        return false;
    }
    if (!--found->second) {
        references_.erase(found);
    }
    --total_;
    return true;
}

std::size_t LibraryReferences::count(std::uintptr_t module) const {
    const std::lock_guard guard(mutex_);
    const auto found = references_.find(module);
    return found == references_.end() ? 0 : found->second;
}
} // namespace ambrosia::services
