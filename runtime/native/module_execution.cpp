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

std::uint64_t Image::run_bootstrap() const {
    auto execution = start();
    return execution.run_bootstrap();
}

Image::Execution::Execution(std::shared_ptr<State> state)
    : state_(std::move(state)), owner_(std::this_thread::get_id()) {
    if (state_->active.exchange(true)) {
        throw std::runtime_error("module already has an active execution session");
    }
    try {
        if (state_->has_tls) {
            thread_.install(0, state_->tls);
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
    if (state_->has_tls) {
        auto scope = thread_.activate();
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
    if (state_->has_tls) {
        auto scope = thread_.activate();
        return entry();
    }
    return entry();
}

std::span<std::byte> Image::Execution::tls_block() {
    require_open();
    return thread_.block(0);
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
