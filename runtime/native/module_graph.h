#pragma once
#include "pe_image.h"
#include <optional>

namespace ambrosia::pe {
using ModuleProvider = std::function<std::optional<std::vector<std::byte>>(const std::string &)>;

struct ModuleStatistics {
    std::size_t modules = 0;
    std::size_t mapped_bytes = 0;
    std::size_t imports = 0;
    bool executable_root = false;
};

struct ModuleLimits {
    ImageLimits image;
    std::size_t source_bytes = 64 * 1024 * 1024;
    std::size_t mapped_bytes = 128 * 1024 * 1024;
};

class ModuleGraph {
    struct State;

  public:
    class Execution;
    explicit ModuleGraph(Resolver external = {}, ModuleProvider provider = {}, ModuleLimits limits = {});
    void add(std::string name, std::span<const std::byte> source);
    void load(std::string root);
    std::uintptr_t export_address(std::string module, const Import &symbol);
    std::vector<std::string> initialization_order() const;
    ModuleStatistics statistics() const;
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
