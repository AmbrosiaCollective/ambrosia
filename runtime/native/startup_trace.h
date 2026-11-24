// Fail-stop diagnostics, never a substitute for guest service implementations.
#pragma once
#include "module_graph.h"
#include <map>
#include <string_view>

namespace ambrosia::diagnostics {
void write_trace(std::string_view message) noexcept;
[[noreturn]] void stop_trace(std::string_view message, int exit_code) noexcept;

class ImportStops {
  public:
    ImportStops();
    ImportStops(const ImportStops &) = delete;
    ImportStops &operator=(const ImportStops &) = delete;
    ImportStops(ImportStops &&) = delete;
    ImportStops &operator=(ImportStops &&) = delete;
    std::uintptr_t resolve(const pe::Import &symbol);
    void seal();
    std::size_t size() const;
    std::string_view label(std::size_t index) const noexcept;

  private:
    friend class ImportStopScope;
    host::Mapping code_;
    std::map<std::string, std::size_t> indices_;
    std::vector<std::string> labels_;
    bool sealed_ = false;
};

class ImportStopScope {
  public:
    explicit ImportStopScope(const ImportStops &stops);
    ~ImportStopScope();
    ImportStopScope(const ImportStopScope &) = delete;
    ImportStopScope &operator=(const ImportStopScope &) = delete;

  private:
    const ImportStops *saved_;
};

// Linux-only fault diagnostics. Owns alternate signal stack and restores prior
// dispositions on normal scope exit. Fault/import stops terminate, never resume.
class FaultScope {
  public:
    explicit FaultScope(std::vector<pe::ModuleRange> ranges);
    ~FaultScope();
    FaultScope(const FaultScope &) = delete;
    FaultScope &operator=(const FaultScope &) = delete;

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace ambrosia::diagnostics
