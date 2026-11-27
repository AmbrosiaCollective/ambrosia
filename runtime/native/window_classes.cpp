#include "window_classes.h"
#include "memory_services.h"
#include <algorithm>
#include <stdexcept>

namespace ambrosia::services {
namespace {
bool same_name(std::string_view first, std::string_view second) {
    const auto fold = [](unsigned char value) {
        return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
    };
    return first.size() == second.size() &&
           std::equal(first.begin(), first.end(), second.begin(),
                      [&](auto left, auto right) { return fold(left) == fold(right); });
}
} // namespace

ClassRegistration WindowClasses::add(WindowClassDescription description) {
    if (description.name.empty() || description.name.size() > 256 || !description.procedure ||
        !description.modules || description.class_extra.size() > 40 || description.window_extra_bytes > 40) {
        return {0, error_invalid_parameter};
    }
    if (std::any_of(description.name.begin(), description.name.end(),
                    [](unsigned char value) { return value == 0 || value > 127; })) {
        throw std::runtime_error("non-ASCII window-class identity requires Unicode atom comparison");
    }
    const std::lock_guard lock(mutex_);
    const bool global = description.style & class_global_style;
    std::uint16_t atom = 0;
    for (const auto &registered : classes_) {
        const auto &existing = registered->description;
        if (!same_name(existing.name, description.name)) {
            continue;
        }
        atom = registered->atom;
        const bool existing_global = existing.style & class_global_style;
        if (global == existing_global && (global || description.instance == existing.instance)) {
            return {0, 1410}; // ERROR_CLASS_ALREADY_EXISTS.
        }
    }
    if (classes_.size() == 128 || (!atom && next_atom_ > UINT16_MAX)) {
        return {0, error_not_enough_memory};
    }
    if (!atom) {
        atom = static_cast<std::uint16_t>(next_atom_);
    }
    auto entry = std::make_shared<WindowClass>(WindowClass{atom, std::move(description)});
    classes_.push_back(std::move(entry));
    if (atom == next_atom_) {
        ++next_atom_;
    }
    return {atom, 0};
}

template <class Match>
std::shared_ptr<const WindowClass> WindowClasses::find_locked(Match match, std::uintptr_t instance) const {
    std::shared_ptr<const WindowClass> global;
    for (const auto &registered : classes_) {
        const auto &description = registered->description;
        if (!match(*registered)) {
            continue;
        }
        if (description.style & class_global_style) {
            global = registered;
        } else if (description.instance == instance) {
            return registered;
        }
    }
    return global;
}

std::shared_ptr<const WindowClass> WindowClasses::find(std::string_view name, std::uintptr_t instance) const {
    const std::lock_guard lock(mutex_);
    return find_locked([&](const auto &registered) { return same_name(registered.description.name, name); },
                       instance);
}

std::shared_ptr<const WindowClass> WindowClasses::find(std::uint16_t atom, std::uintptr_t instance) const {
    const std::lock_guard lock(mutex_);
    return find_locked([&](const auto &registered) { return registered.atom == atom; }, instance);
}
} // namespace ambrosia::services
