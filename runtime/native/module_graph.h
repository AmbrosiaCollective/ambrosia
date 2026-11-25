#pragma once
#include "pe_image.h"
#include <filesystem>
#include <optional>

namespace ambrosia::pe {
using ModuleProvider = std::function<std::optional<std::vector<std::byte>>(const std::string &)>;

using ModulePathProvider = std::function<std::optional<std::filesystem::path>(const std::string &)>;

struct ModuleStatistics {
    std::size_t modules = 0;
    std::size_t mapped_bytes = 0;
    std::size_t imports = 0;
    std::size_t unresolved_imports = 0;
    std::size_t unwind_records = 0;
    std::uint32_t unsupported_directories = 0;
    bool inspection_only = false;
    bool executable_root = false;
};

struct ModuleLimits {
    ImageLimits image;
    std::size_t source_bytes = 64 * 1024 * 1024;
    std::size_t mapped_bytes = 128 * 1024 * 1024;
};

struct ModuleRange {
    std::string name;
    std::uintptr_t base = 0;
    std::size_t bytes = 0;
};

class ModuleGraph {
    struct State;

  public:
    class Execution;
    explicit ModuleGraph(Resolver external = {}, ModuleProvider provider = {}, ModuleLimits limits = {},
                         Resolver diagnostic_stop = {}, ModulePathProvider source_paths = {});
    void add(std::string name, std::span<const std::byte> source,
             std::optional<std::filesystem::path> path = std::nullopt);
    void load(std::string root);
    std::uintptr_t export_address(std::string module, const Import &symbol);
    std::vector<std::string> initialization_order() const;
    ModuleStatistics statistics() const;
    std::vector<ModuleRange> diagnostic_ranges() const;
    std::uintptr_t module_base_for_address(std::uintptr_t address) const;
    std::uintptr_t loaded_module(std::optional<std::string> name = std::nullopt) const;
    std::optional<std::filesystem::path> loaded_module_path(std::uintptr_t module = 0) const;
    std::optional<std::uintptr_t> loaded_export(std::uintptr_t module, const Import &symbol) const;
    bool disable_thread_notifications(std::uintptr_t module);
    std::optional<bool> thread_notifications_enabled(std::uintptr_t module) const;
    Execution start() const;

  private:
    std::shared_ptr<State> state_;
};

class ModuleGraph::Execution {
  public:
    ~Execution();
    Execution(const Execution &) = delete;
    Execution &operator=(const Execution &) = delete;
    Execution(Execution &&) = delete;
    Execution &operator=(Execution &&) = delete;
    std::uint64_t run_bootstrap();
    void with_guest_context(const std::function<void()> &callback);
    void close();

  private:
    friend class ModuleGraph;
    explicit Execution(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
    std::shared_ptr<guest::ThreadContext> thread_;
    std::vector<std::unique_ptr<Image::Execution>> sessions_;
    bool closed_ = true;
};
} // namespace ambrosia::pe
