// Bundled dependency ordering and forwarder resolution adapted from the
// inherited runtime/src/pe_loader.cpp at 17176bd; GPL-3.0-or-later.
#include "module_graph.h"
#include "image_state.h"
#include <charconv>
#include <optional>
#include <set>
#include <stdexcept>

namespace ambrosia::pe {
namespace {
std::string module_key(std::string name) {
    if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\:") != std::string::npos) {
        throw std::runtime_error("module name must be a basename");
    }
    for (auto &character : name) {
        if (static_cast<unsigned char>(character) < 32 || character == 127) {
            throw std::runtime_error("module name contains a control character");
        }
        if (character >= 'A' && character <= 'Z') {
            character += 'a' - 'A';
        }
    }
    if (name.back() == '.') {
        name.pop_back();
    } else if (name.find('.') == std::string::npos) {
        name += ".dll";
    }
    return name;
}

Import forward_target(const std::string &text) {
    const auto dot = text.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == text.size()) {
        throw std::runtime_error("invalid export forwarder");
    }
    Import target;
    target.module = module_key(text.substr(0, dot));
    target.name = text.substr(dot + 1);
    if (target.name.front() == '#') {
        unsigned ordinal = 0;
        const auto *begin = target.name.data() + 1;
        const auto *end = target.name.data() + target.name.size();
        const auto parsed = std::from_chars(begin, end, ordinal);
        if (parsed.ec != std::errc{} || parsed.ptr != end || ordinal > 65535) {
            throw std::runtime_error("invalid forwarded ordinal");
        }
        target.ordinal = static_cast<std::uint16_t>(ordinal);
        target.by_ordinal = true;
        target.name.clear();
    }
    return target;
}
} // namespace

struct ModuleGraph::State {
    struct Node {
        std::vector<std::byte> source;
        std::optional<Image> image;
        std::set<std::string> dependencies;
        bool loading = false;
    };

    std::map<std::string, Node> modules;
    std::vector<std::string> order;
    std::string root;
    Resolver external;
    Resolver diagnostic_stop;
    ModuleProvider provider;
    ModuleLimits limits;
    std::thread::id owner = std::this_thread::get_id();
    std::size_t source_bytes = 0;
    std::size_t mapped_bytes = 0;
    std::uint32_t next_slot = 0;
    bool active = false;
    bool failed = false;

    void register_source(std::string name, std::span<const std::byte> source) {
        if (modules.size() >= 64 || source.size() > limits.source_bytes - source_bytes ||
            modules.contains(name)) {
            throw std::runtime_error("module registration is ambiguous or exceeds bootstrap budget");
        }
        Node node;
        node.source.assign(source.begin(), source.end());
        modules.emplace(std::move(name), std::move(node));
        source_bytes += source.size();
    }

    void require_owner() const {
        if (owner != std::this_thread::get_id() || failed) {
            throw std::runtime_error("module graph is failed or belongs to another thread");
        }
    }

    Image &load_module(const std::string &name) {
        auto &node = modules.at(name);
        if (node.image) {
            return *node.image;
        }
        if (active || node.loading) {
            throw std::runtime_error("new or cyclic module dependency cannot be loaded here");
        }
        node.loading = true;
        const auto slot = next_slot++;
        auto resolve_import = [&](const Import &symbol) {
            const auto dependency = module_key(symbol.module);
            std::set<std::pair<std::string, std::string>> visited;
            auto address = resolve(dependency, symbol, visited);
            if (!address && diagnostic_stop) {
                address = diagnostic_stop(symbol);
            }
            if (modules.contains(dependency)) {
                node.dependencies.insert(dependency);
            }
            return address;
        };
        auto image_limits = limits.image;
        image_limits.mapped_bytes = std::min(image_limits.mapped_bytes, limits.mapped_bytes - mapped_bytes);
        auto image = Image::load(node.source, resolve_import, slot, image_limits);
        if (image.size() > limits.mapped_bytes - mapped_bytes) {
            throw std::runtime_error("module mappings exceed bootstrap budget");
        }
        mapped_bytes += image.size();
        node.image.emplace(std::move(image));
        node.loading = false;
        return *node.image;
    }

    std::uintptr_t resolve(const std::string &module, const Import &symbol,
                           std::set<std::pair<std::string, std::string>> &visited) {
        const auto identity =
            symbol.by_ordinal ? "ordinal:" + std::to_string(symbol.ordinal) : "name:" + symbol.name;
        if (visited.size() >= 64 || !visited.emplace(module, identity).second) {
            throw std::runtime_error("export forwarder cycle or depth limit");
        }
        if (!modules.contains(module)) {
            Import request = symbol;
            request.module = module;
            const auto address = external ? external(request) : 0;
            if (address) {
                return address;
            }
            if (!provider || active) {
                return 0;
            }
            auto bytes = provider(module);
            if (!bytes) {
                return 0;
            }
            register_source(module, *bytes);
        }
        auto &image = load_module(module);
        const auto &exports = image.state_->exports;
        const ExportTarget *target = nullptr;
        if (symbol.by_ordinal) {
            auto found = exports.ordinals.find(symbol.ordinal);
            if (found != exports.ordinals.end()) {
                target = &found->second;
            }
        } else {
            auto found = exports.names.find(symbol.name);
            if (found != exports.names.end()) {
                target = &found->second;
            }
        }
        if (!target) {
            return 0;
        }
        if (target->forwarder.empty()) {
            return reinterpret_cast<std::uintptr_t>(image.base() + target->rva);
        }
        const auto next = forward_target(target->forwarder);
        const auto address = resolve(next.module, next, visited);
        if (next.module != module && modules.contains(next.module)) {
            modules.at(module).dependencies.insert(next.module);
        }
        return address;
    }

    void update_order() {
        order.clear();
        std::set<std::string> visiting, complete;
        std::function<void(const std::string &)> visit = [&](const std::string &name) {
            if (complete.contains(name)) {
                return;
            }
            if (!visiting.insert(name).second) {
                throw std::runtime_error("cyclic module initialization is not implemented");
            }
            for (const auto &dependency : modules.at(name).dependencies) {
                visit(dependency);
            }
            visiting.erase(name);
            complete.insert(name);
            order.push_back(name);
        };
        visit(root);
    }
};

ModuleGraph::ModuleGraph(Resolver external, ModuleProvider provider, ModuleLimits limits,
                         Resolver diagnostic_stop)
    : state_(std::make_shared<State>()) {
    if (!limits.source_bytes || !limits.mapped_bytes || !limits.image.file_bytes ||
        !limits.image.mapped_bytes) {
        throw std::invalid_argument("module byte budgets must be nonzero");
    }
    state_->limits = limits;
    if (diagnostic_stop && limits.image.mode != LoadMode::diagnostic_startup) {
        throw std::invalid_argument("diagnostic import stops require diagnostic startup mode");
    }
    state_->diagnostic_stop = std::move(diagnostic_stop);
    state_->external = std::move(external);
    state_->provider = std::move(provider);
}

void ModuleGraph::add(std::string name, std::span<const std::byte> source) {
    state_->require_owner();
    if (!state_->root.empty()) {
        throw std::runtime_error("module registration is sealed");
    }
    state_->register_source(module_key(std::move(name)), source);
}

void ModuleGraph::load(std::string root) {
    state_->require_owner();
    if (!state_->root.empty()) {
        throw std::runtime_error("module graph is already loaded");
    }
    state_->root = module_key(std::move(root));
    try {
        state_->load_module(state_->root);
        state_->update_order();
    } catch (...) {
        state_->failed = true;
        throw;
    }
}

std::uintptr_t ModuleGraph::export_address(std::string module, const Import &symbol) {
    state_->require_owner();
    if (state_->root.empty() || state_->active) {
        throw std::runtime_error("export lookup requires a loaded inactive graph");
    }
    std::set<std::pair<std::string, std::string>> visited;
    try {
        const auto address = state_->resolve(module_key(std::move(module)), symbol, visited);
        state_->update_order();
        return address;
    } catch (...) {
        state_->failed = true;
        throw;
    }
}

std::vector<std::string> ModuleGraph::initialization_order() const {
    state_->require_owner();
    return state_->order;
}

ModuleStatistics ModuleGraph::statistics() const {
    state_->require_owner();
    ModuleStatistics result;
    result.inspection_only = state_->limits.image.mode == LoadMode::inspect;
    result.mapped_bytes = state_->mapped_bytes;
    for (const auto &[name, node] : state_->modules) {
        (void)name;
        if (node.image) {
            ++result.modules;
            result.imports += node.image->imports().size();
            result.unresolved_imports += node.image->state_->unresolved_imports;
            result.unwind_records += node.image->state_->unwind.entries().size();
            result.unsupported_directories |= node.image->state_->unsupported_directories;
            if (name == state_->root) {
                result.executable_root = !node.image->state_->dll;
            }
        }
    }
    return result;
}

std::vector<ModuleRange> ModuleGraph::diagnostic_ranges() const {
    state_->require_owner();
    std::vector<ModuleRange> ranges;
    for (const auto &[name, node] : state_->modules) {
        if (node.image) {
            ranges.push_back(
                {name, reinterpret_cast<std::uintptr_t>(node.image->base()), node.image->size()});
        }
    }
    return ranges;
}

std::uintptr_t ModuleGraph::module_base_for_address(std::uintptr_t address) const {
    state_->require_owner();
    for (const auto &[name, node] : state_->modules) {
        (void)name;
        if (node.image) {
            const auto base = reinterpret_cast<std::uintptr_t>(node.image->base());
            // Subtraction avoids wraparound at the upper address limit. Image
            // containment includes headers/gaps, not just executable sections.
            if (address >= base && address - base < node.image->size()) {
                return base;
            }
        }
    }
    return 0;
}

bool ModuleGraph::disable_thread_notifications(std::uintptr_t module) {
    state_->require_owner();
    for (auto &[name, node] : state_->modules) {
        (void)name;
        if (node.image && reinterpret_cast<std::uintptr_t>(node.image->base()) == module) {
            auto &image = *node.image->state_;
            if (!image.dll || image.has_tls || image.inspection_only) {
                return false;
            }
            image.thread_notifications = false;
            return true;
        }
    }
    return false;
}

std::optional<bool> ModuleGraph::thread_notifications_enabled(std::uintptr_t module) const {
    state_->require_owner();
    for (const auto &[name, node] : state_->modules) {
        (void)name;
        if (node.image && reinterpret_cast<std::uintptr_t>(node.image->base()) == module) {
            return node.image->state_->thread_notifications;
        }
    }
    return std::nullopt;
}

ModuleGraph::Execution ModuleGraph::start() const {
    state_->require_owner();
    if (state_->limits.image.mode == LoadMode::inspect) {
        throw std::runtime_error("inspection-only module graph cannot execute");
    }
    if (state_->order.empty()) {
        throw std::runtime_error("module graph is not loaded");
    }
    return Execution(state_);
}

ModuleGraph::Execution::Execution(std::shared_ptr<State> state)
    : state_(std::move(state)), thread_(std::make_shared<guest::ThreadContext>()) {
    if (state_->active) {
        throw std::runtime_error("module graph already has an active session");
    }
    state_->active = true;
    closed_ = false;
    try {
        for (const auto &name : state_->order) {
            auto &image = *state_->modules.at(name).image;
            if (image.state_->has_tls) {
                thread_->install(image.state_->tls_slot, image.state_->tls);
            }
        }
        for (const auto &name : state_->order) {
            sessions_.push_back(state_->modules.at(name).image->start_in(thread_));
        }
    } catch (...) {
        close();
        throw;
    }
}

ModuleGraph::Execution::~Execution() {
    try {
        close();
    } catch (...) {
        std::terminate();
    }
}

std::uint64_t ModuleGraph::Execution::run_bootstrap() {
    state_->require_owner();
    if (closed_) {
        throw std::runtime_error("module graph session is closed");
    }
    return sessions_.back()->run_bootstrap();
}

void ModuleGraph::Execution::close() {
    if (closed_) {
        return;
    }
    state_->require_owner();
    for (auto session = sessions_.rbegin(); session != sessions_.rend(); ++session) {
        (*session)->close();
    }
    sessions_.clear();
    closed_ = true;
    state_->active = false;
}
} // namespace ambrosia::pe
