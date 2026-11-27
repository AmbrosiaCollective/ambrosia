#include "title_memory.h"
#include "guest_thread.h"
#include "module_graph.h"
#include "service_runtime.h"
#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace ambrosia::services {
namespace {
struct PreserveErrno {
    int saved = errno;

    ~PreserveErrno() {
        errno = saved;
    }
};

void require_context(Runtime *runtime, pe::ModuleGraph *modules, std::uint64_t generation) {
    if (!runtime || !modules || current_runtime() != runtime || current_modules() != modules ||
        !guest::ThreadContext::current() || modules->execution_generation() != generation) {
        throw std::runtime_error(
            "title allocation requires its original active guest runtime and module generation");
    }
}

std::array<std::uintptr_t, 2> allocation_hooks(pe::ModuleGraph &modules) {
    std::uintptr_t table = 0;
    pe::Import symbol;
    symbol.name = "__XMemAllocationHooks__";
    for (const auto &name : modules.initialization_order()) {
        const auto module = modules.loaded_module(name);
        const auto address = modules.loaded_export(module, symbol);
        if (address && *address) {
            if (table && table != *address) {
                throw std::runtime_error("title allocation hooks are ambiguous across loaded modules");
            }
            table = *address;
        }
    }
    std::array<std::uintptr_t, 2> hooks{};
    if (!table || !modules.copy_loaded_data(table, std::as_writable_bytes(std::span(hooks))) ||
        !modules.active_code_address(hooks[0]) || !modules.active_code_address(hooks[1])) {
        throw std::runtime_error("title allocation hooks require readable owned data and active loaded code");
    }
    return hooks;
}
} // namespace

TitleMemoryAllocation::TitleMemoryAllocation(TitleMemoryAllocation &&other) noexcept
    : runtime_(other.runtime_), modules_(other.modules_), generation_(other.generation_), free_(other.free_),
      data_(std::exchange(other.data_, nullptr)), bytes_(std::exchange(other.bytes_, 0)) {}

void *TitleMemoryAllocation::data() const {
    return data_;
}

std::size_t TitleMemoryAllocation::size() const {
    return bytes_;
}

void TitleMemoryAllocation::close() {
    const PreserveErrno preserve;
    if (!data_) {
        return;
    }
    require_context(runtime_, modules_, generation_);
    if (!modules_->active_code_address(free_)) {
        throw std::runtime_error("title allocation release callback is no longer active loaded code");
    }
    using Free = void(AMBROSIA_GUEST_ABI *)(void *, std::uint64_t);
    // Retire local ownership before a potentially reentrant title callback.
    auto *address = std::exchange(data_, nullptr);
    bytes_ = 0;
    reinterpret_cast<Free>(free_)(address, descriptor_memory_attributes);
}

TitleMemoryAllocation allocate_title_descriptors(std::size_t bytes) {
    const PreserveErrno preserve;
    auto *runtime = current_runtime();
    auto *modules = current_modules();
    if (!runtime || !modules || !guest::ThreadContext::current() || !bytes || bytes > 64 * 1024 * 1024) {
        throw std::runtime_error(
            "title descriptor allocation requires active guest scope and a bounded size");
    }
    const auto generation = modules->execution_generation();
    require_context(runtime, modules, generation);
    const auto hooks = allocation_hooks(*modules);
    using Allocate = void *(AMBROSIA_GUEST_ABI *)(std::size_t, std::uint64_t);
    // This runs synchronously inside the already active guest context. Never
    // hold a graphics ownership lock across a reentrant title allocator call.
    auto *address = reinterpret_cast<Allocate>(hooks[0])(bytes, descriptor_memory_attributes);
    if (!address) {
        throw std::runtime_error("title descriptor allocator returned no storage");
    }
    TitleMemoryAllocation result;
    result.runtime_ = runtime;
    result.modules_ = modules;
    result.generation_ = generation;
    result.free_ = hooks[1];
    result.data_ = address;
    result.bytes_ = bytes;
    return result;
}
} // namespace ambrosia::services
