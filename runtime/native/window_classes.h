#pragma once
#include "import_registry.h"
#include "module_graph.h"
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ambrosia::services {
inline constexpr std::uint32_t class_global_style = 0x4000;
using ClassMenu = std::variant<std::monostate, std::uint16_t, std::string>;

struct WindowClassDescription {
    std::string name;
    std::uint32_t style = 0;
    std::uintptr_t procedure = 0;
    std::uintptr_t instance = 0;
    std::uintptr_t icon = 0;
    std::uintptr_t cursor = 0;
    std::uintptr_t background = 0;
    std::uintptr_t small_icon = 0;
    std::uint32_t window_extra_bytes = 0;
    ClassMenu menu;
    std::vector<std::byte> class_extra;
    // Registration never invokes the procedure. Retain its mapped catalog for
    // future window/message owners; do not retain a caller's temporary Scope.
    std::shared_ptr<const pe::ModuleGraph> modules;
};

struct WindowClass {
    std::uint16_t atom = 0;
    WindowClassDescription description;
};

struct ClassRegistration {
    std::uint16_t atom = 0;
    std::uint32_t error = 0;
};

class WindowClasses {
  public:
    ClassRegistration add(WindowClassDescription description);
    std::shared_ptr<const WindowClass> find(std::string_view name, std::uintptr_t instance) const;
    std::shared_ptr<const WindowClass> find(std::uint16_t atom, std::uintptr_t instance) const;

  private:
    template <class Match>
    std::shared_ptr<const WindowClass> find_locked(Match match, std::uintptr_t instance) const;
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<const WindowClass>> classes_;
    std::uint32_t next_atom_ = 0xc000;
};

void register_window_class_services(ImportRegistry &registry);
} // namespace ambrosia::services
