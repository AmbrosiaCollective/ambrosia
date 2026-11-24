#pragma once
#include "critical_sections.h"
#include "fls_slots.h"
#include "import_registry.h"
#include "memory_services.h"

namespace ambrosia::services {
std::uint32_t get_last_error() noexcept;
void set_last_error(std::uint32_t error) noexcept;

class Runtime {
  public:
    explicit Runtime(std::size_t memory_budget = 64 * 1024 * 1024);
    std::uintptr_t resolve(const pe::Import &symbol) const;

    MemoryServices &memory() {
        return memory_;
    }

    CriticalSections &critical_sections() {
        return critical_sections_;
    }

    FlsSlots &fls() {
        return fls_;
    }

  private:
    MemoryServices memory_;
    CriticalSections critical_sections_;
    FlsSlots fls_;
    ImportRegistry registry_;
};

Runtime *current_runtime() noexcept;

// The host-side service context is scoped independently of guest GS/TEB state.
// It must enclose module initialization, entry and detach notifications.
class Scope {
  public:
    explicit Scope(Runtime &runtime);
    ~Scope();
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;
    Scope(Scope &&) = delete;
    Scope &operator=(Scope &&) = delete;

  private:
    Runtime *saved_;
};
} // namespace ambrosia::services
