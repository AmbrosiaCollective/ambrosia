#include "startup_trace.h"
#include <array>
#include <atomic>
#include <exception>
#include <iterator>
#include <stdexcept>
#if defined(__linux__) && defined(__x86_64__)
#include <csignal>
#include <ucontext.h>
#include <unistd.h>
#endif

namespace ambrosia::diagnostics {
#if defined(__linux__) && defined(__x86_64__)
namespace {
std::atomic<const std::vector<pe::ModuleRange> *> active_ranges = nullptr;
static_assert(decltype(active_ranges)::is_always_lock_free);
constexpr int signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};

void emit(std::string_view text) noexcept {
    write_trace(text);
}

void emit_hex(std::uintptr_t value) noexcept {
    constexpr char digits[] = "0123456789abcdef";
    char buffer[2 + sizeof(value) * 2] = {'0', 'x'};
    for (std::size_t index = 0; index < sizeof(value) * 2; ++index) {
        const auto shift = (sizeof(value) * 2 - index - 1) * 4;
        buffer[index + 2] = digits[(value >> shift) & 15];
    }
    emit(std::string_view(buffer, sizeof(buffer)));
}

void fault(int signal, siginfo_t *info, void *raw_context) noexcept {
    const auto *context = static_cast<ucontext_t *>(raw_context);
    const auto pc = static_cast<std::uintptr_t>(context->uc_mcontext.gregs[REG_RIP]);
    emit("TRACE fault signal=");
    emit_hex(static_cast<unsigned>(signal));
    emit(" address=");
    emit_hex(reinterpret_cast<std::uintptr_t>(info->si_addr));
    bool located = false;
    const auto *ranges = active_ranges.load(std::memory_order_relaxed);
    if (ranges) {
        for (const auto &module : *ranges) {
            if (pc >= module.base && pc - module.base < module.bytes) {
                emit(" module=");
                emit(module.name);
                emit(" rva=");
                emit_hex(pc - module.base);
                located = true;
                break;
            }
        }
    }
    if (!located) {
        emit(" outside_guest_modules");
    }
    stop_trace("", 79);
}
} // namespace

struct FaultScope::State {
    std::vector<pe::ModuleRange> ranges;
    host::Mapping signal_stack{64 * 1024};
    stack_t previous_stack{};
    std::array<struct sigaction, std::size(signals)> previous_actions{};
    std::size_t installed = 0;
    bool stack_installed = false;
    const std::vector<pe::ModuleRange> *saved_ranges = nullptr;

    ~State() {
        for (std::size_t index = 0; index < installed; ++index) {
            if (sigaction(signals[index], &previous_actions[index], nullptr)) {
                std::terminate();
            }
        }
        if (stack_installed && sigaltstack(&previous_stack, nullptr)) {
            std::terminate();
        }
        active_ranges.store(saved_ranges, std::memory_order_relaxed);
    }
};

FaultScope::FaultScope(std::vector<pe::ModuleRange> ranges) : state_(std::make_unique<State>()) {
    state_->ranges = std::move(ranges);
    state_->saved_ranges = active_ranges.load(std::memory_order_relaxed);
    stack_t stack{};
    stack.ss_sp = state_->signal_stack.data();
    stack.ss_size = state_->signal_stack.size();
    if (sigaltstack(&stack, &state_->previous_stack)) {
        throw std::runtime_error("cannot install diagnostic alternate signal stack");
    }
    state_->stack_installed = true;

    struct sigaction action {};

    action.sa_sigaction = &fault;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (const auto signal : signals) {
        if (sigaction(signal, &action, &state_->previous_actions[state_->installed])) {
            throw std::runtime_error("cannot install diagnostic fault handler");
        }
        ++state_->installed;
    }
    active_ranges.store(&state_->ranges, std::memory_order_relaxed);
}

struct ThreadFaultStack::State {
    host::Mapping memory{64 * 1024};
    stack_t previous{};
    bool installed = false;

    ~State() {
        if (installed && sigaltstack(&previous, nullptr)) {
            std::terminate();
        }
    }
};

ThreadFaultStack::ThreadFaultStack() : state_(std::make_unique<State>()) {
    stack_t stack{};
    stack.ss_sp = state_->memory.data();
    stack.ss_size = state_->memory.size();
    if (sigaltstack(&stack, &state_->previous)) {
        throw std::runtime_error("cannot install worker diagnostic alternate stack");
    }
    state_->installed = true;
}
#else
struct FaultScope::State {};

struct ThreadFaultStack::State {};

ThreadFaultStack::ThreadFaultStack() {
    throw std::runtime_error("worker diagnostic stacks require native x64 Linux");
}

FaultScope::FaultScope(std::vector<pe::ModuleRange>) {
    throw std::runtime_error("diagnostic fault tracing requires native x64 Linux");
}
#endif

FaultScope::~FaultScope() = default;
ThreadFaultStack::~ThreadFaultStack() = default;
} // namespace ambrosia::diagnostics
