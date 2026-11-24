#include "startup_trace.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#ifdef __linux__
#include <unistd.h>
#endif

namespace ambrosia::diagnostics {
namespace {
std::atomic<const ImportStops *> active_stops = nullptr;
static_assert(decltype(active_stops)::is_always_lock_free);
constexpr std::size_t stop_limit = 4096;
constexpr std::size_t instruction_bytes = 32;

std::string safe_label(const pe::Import &symbol) {
    auto result =
        symbol.module + "!" + (symbol.by_ordinal ? "#" + std::to_string(symbol.ordinal) : symbol.name);
    if (result.empty() || result.size() > 1024) {
        throw std::runtime_error("diagnostic import label exceeds budget");
    }
    for (auto &character : result) {
        const auto value = static_cast<unsigned char>(character);
        if (value < 32 || value > 126) {
            throw std::runtime_error("diagnostic import label contains non-printable bytes");
        }
    }
    return result;
}

[[noreturn]] void missing_import(std::uint32_t index) noexcept {
    const auto *stops = active_stops.load(std::memory_order_relaxed);
    if (!stops || index >= stops->size()) {
        stop_trace("TRACE invalid import-stop context\n", 78);
    }
    const auto label = stops->label(index);
    stop_trace(label, 78);
}
} // namespace

void write_trace(std::string_view message) noexcept {
#ifdef __linux__
    while (!message.empty()) {
        const auto written = write(STDERR_FILENO, message.data(), message.size());
        if (written <= 0) {
            break;
        }
        message.remove_prefix(static_cast<std::size_t>(written));
    }
#else
    (void)std::fwrite(message.data(), 1, message.size(), stderr);
    (void)std::fflush(stderr);
#endif
}

[[noreturn]] void stop_trace(std::string_view message, int exit_code) noexcept {
    write_trace(message);
    write_trace("\n");
    std::_Exit(exit_code);
}

ImportStops::ImportStops() : code_(stop_limit * instruction_bytes) {}

std::uintptr_t ImportStops::resolve(const pe::Import &symbol) {
    if (sealed_) {
        throw std::runtime_error("diagnostic import stops are sealed");
    }
    const auto name = safe_label(symbol);
    if (const auto found = indices_.find(name); found != indices_.end()) {
        return reinterpret_cast<std::uintptr_t>(code_.data() + found->second * instruction_bytes);
    }
    if (labels_.size() >= stop_limit) {
        throw std::runtime_error("diagnostic import-stop budget exhausted");
    }
    const auto index = static_cast<std::uint32_t>(labels_.size());
    auto *entry = code_.data() + index * instruction_bytes;
    // A non-returning bridge replaces only the first argument, then tail-jumps
    // to the host ABI stop function. Guest register preservation is irrelevant:
    // the process always exits. No address is writable and executable together.
#ifdef _WIN32
    entry[0] = std::byte{0xb9}; // mov ecx,index (Windows host ABI)
#else
    entry[0] = std::byte{0xbf}; // mov edi,index (System V host ABI)
#endif
    std::memcpy(entry + 1, &index, sizeof(index));
    entry[5] = std::byte{0x48};
    entry[6] = std::byte{0xb8};
    const auto target = reinterpret_cast<std::uintptr_t>(&missing_import);
    std::memcpy(entry + 7, &target, sizeof(target));
    entry[15] = std::byte{0xff};
    entry[16] = std::byte{0xe0};
    labels_.push_back("TRACE unsupported import " + name);
    indices_.emplace(name, index);
    return reinterpret_cast<std::uintptr_t>(entry);
}

void ImportStops::seal() {
    if (sealed_) {
        throw std::runtime_error("diagnostic import stops are already sealed");
    }
    code_.flush_instructions();
    code_.protect(0, code_.size(), host::Access::read_execute);
    sealed_ = true;
}

std::size_t ImportStops::size() const {
    return labels_.size();
}

std::string_view ImportStops::label(std::size_t index) const noexcept {
    return index < labels_.size() ? std::string_view(labels_[index]) : std::string_view{};
}

ImportStopScope::ImportStopScope(const ImportStops &stops) : saved_(nullptr) {
    if (!stops.sealed_) {
        throw std::runtime_error("diagnostic import stops must be sealed before activation");
    }
    saved_ = active_stops.exchange(&stops, std::memory_order_relaxed);
}

ImportStopScope::~ImportStopScope() {
    active_stops.store(saved_, std::memory_order_relaxed);
}
} // namespace ambrosia::diagnostics
