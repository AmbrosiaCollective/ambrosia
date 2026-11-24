#include "image_state.h"
#include <exception>
#include <stdexcept>

namespace ambrosia::pe {
namespace {
using Entry = std::uint64_t(AMBROSIA_GUEST_ABI *)();
using TlsCallback = void(AMBROSIA_GUEST_ABI *)(void *, std::uint32_t, void *);
using DllEntry = std::int32_t(AMBROSIA_GUEST_ABI *)(void *, std::uint32_t, void *);
constexpr std::uint32_t process_detach = 0;
constexpr std::uint32_t process_attach = 1;
} // namespace

Image::Image(std::size_t size) : state_(std::make_shared<State>(size)) {}

const std::byte *Image::base() const {
    return state_ ? state_->memory.data() : nullptr;
}

std::size_t Image::size() const {
    return state_ ? state_->memory.size() : 0;
}

const std::vector<Import> &Image::imports() const {
    if (!state_) {
        throw std::runtime_error("image is not mapped");
    }
    return state_->imports;
}

Image::Execution Image::start() const {
    if (!state_) {
        throw std::runtime_error("image is not mapped");
    }
    return Execution(state_);
}

std::unique_ptr<Image::Execution> Image::start_in(std::shared_ptr<guest::ThreadContext> context) const {
    return std::unique_ptr<Execution>(new Execution(state_, std::move(context)));
}

std::uintptr_t Image::export_address(const Import &symbol) const {
    if (!state_) {
        throw std::runtime_error("image is not mapped");
    }
    const ExportTarget *target = nullptr;
    if (symbol.by_ordinal) {
        auto found = state_->exports.ordinals.find(symbol.ordinal);
        if (found != state_->exports.ordinals.end()) {
            target = &found->second;
        }
    } else {
        auto found = state_->exports.names.find(symbol.name);
        if (found != state_->exports.names.end()) {
            target = &found->second;
        }
    }
    if (!target) {
        return 0;
    }
    if (!target->forwarder.empty()) {
        throw std::runtime_error("forwarded export requires a module graph");
    }
    return reinterpret_cast<std::uintptr_t>(state_->memory.data() + target->rva);
}

std::uint64_t Image::run_bootstrap() const {
    auto execution = start();
    return execution.run_bootstrap();
}

Image::Execution::Execution(std::shared_ptr<State> state, std::shared_ptr<guest::ThreadContext> context)
    : state_(std::move(state)), thread_(std::move(context)), owner_(std::this_thread::get_id()) {
    if (state_->inspection_only) {
        throw std::runtime_error("inspection-only image cannot execute");
    }
    if (state_->active.exchange(true)) {
        throw std::runtime_error("module already has an active execution session");
    }
    try {
        const bool shared_context = bool(thread_);
        needs_context_ = shared_context || state_->has_tls;
        if (!thread_) {
            thread_ = std::make_shared<guest::ThreadContext>();
        }
        if (state_->has_tls) {
            if (shared_context) {
                (void)thread_->block(state_->tls_slot);
            } else {
                thread_->install(state_->tls_slot, state_->tls);
            }
        }
        closed_ = false;
        if (!notify(process_attach)) {
            close();
            throw std::runtime_error("DLL process attach returned false");
        }
    } catch (...) {
        closed_ = true;
        state_->active = false;
        throw;
    }
}

Image::Execution::~Execution() {
    try {
        close();
    } catch (...) {
        std::terminate();
    }
}

void Image::Execution::require_open() const {
    if (closed_ || owner_ != std::this_thread::get_id()) {
        throw std::runtime_error("module session is closed or belongs to another thread");
    }
}

bool Image::Execution::notify(std::uint32_t reason) {
    auto dispatch = [&] {
        auto *base = state_->memory.data();
        for (const auto callback_rva : state_->callbacks) {
            auto callback = reinterpret_cast<TlsCallback>(base + callback_rva);
            callback(base, reason, nullptr);
        }
        if (state_->dll && state_->entry) {
            auto entry = reinterpret_cast<DllEntry>(base + state_->entry);
            // Dynamic-load semantics only. Static process initialization needs
            // a real guest startup context rather than a fabricated pointer.
            return entry(base, reason, nullptr) != 0;
        }
        return true;
    };
    if (needs_context_) {
        auto scope = thread_->activate();
        return dispatch();
    }
    return dispatch();
}

std::uint64_t Image::Execution::run_bootstrap() {
    require_open();
    if (state_->dll || !state_->entry) {
        throw std::runtime_error("DLL entry is not a bootstrap executable entry");
    }
    auto entry = reinterpret_cast<Entry>(state_->memory.data() + state_->entry);
    if (needs_context_) {
        auto scope = thread_->activate();
        return entry();
    }
    return entry();
}

std::span<std::byte> Image::Execution::tls_block() {
    require_open();
    return thread_->block(state_->tls_slot);
}

void Image::Execution::close() {
    if (closed_) {
        return;
    }
    require_open();
    (void)notify(process_detach);
    closed_ = true;
    state_->active = false;
}
} // namespace ambrosia::pe
