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
    std::thread::id owner = std::this_thread::get_id();
    std::size_t source_bytes = 0;
    std::size_t mapped_bytes = 0;
    std::uint32_t next_slot = 0;
    bool active = false;
    bool failed = false;

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
            if (modules.contains(dependency)) {
                node.dependencies.insert(dependency);
            }
            std::set<std::pair<std::string, std::string>> visited;
            return resolve(dependency, symbol, visited);
        };
        auto image = Image::load(node.source, resolve_import, slot);
        if (image.size() > 128 * 1024 * 1024 - mapped_bytes) {
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
            return external ? external(request) : 0;
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
        if (next.module != module && modules.contains(next.module)) {
            modules.at(module).dependencies.insert(next.module);
        }
        return resolve(next.module, next, visited);
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

ModuleGraph::ModuleGraph(Resolver external) : state_(std::make_shared<State>()) {
    state_->external = std::move(external);
}

void ModuleGraph::add(std::string name, std::span<const std::byte> source) {
    state_->require_owner();
    if (!state_->root.empty() || state_->modules.size() >= 64 ||
        source.size() > 64 * 1024 * 1024 - state_->source_bytes) {
        throw std::runtime_error("module registration is sealed or exceeds bootstrap budget");
    }
    name = module_key(std::move(name));
    if (state_->modules.contains(name)) {
        throw std::runtime_error("duplicate module basename");
    }
    State::Node node;
    node.source.assign(source.begin(), source.end());
    state_->source_bytes += source.size();
    state_->modules.emplace(std::move(name), std::move(node));
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

} // namespace ambrosia::pe
