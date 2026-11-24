#pragma once
#include "pe_image.h"

namespace ambrosia::pe {
class ModuleGraph {
    struct State;

  public:
    class Execution;
    explicit ModuleGraph(Resolver external = {});
    void add(std::string name, std::span<const std::byte> source);
    void load(std::string root);
    std::uintptr_t export_address(std::string module, const Import &symbol);
    std::vector<std::string> initialization_order() const;
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
