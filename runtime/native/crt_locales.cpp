#include "crt_locales.h"
#include <new>

namespace ambrosia::services {
struct CrtLocales::ThreadState {
    explicit ThreadState(const CrtLocale &initial) : locale(initial) {}

    std::int32_t mode = 2; // _DISABLE_PER_THREAD_LOCALE.
    CrtLocale locale;
};

CrtLocales::CrtLocales() {
    const auto allocation = slots_.allocate(&destroy_thread_state);
    if (allocation.error) {
        throw std::bad_alloc();
    }
    slot_ = allocation.index;
}

void AMBROSIA_GUEST_ABI CrtLocales::destroy_thread_state(void *value) {
    delete static_cast<ThreadState *>(value);
}

CrtLocales::ThreadState &CrtLocales::bind(guest::ThreadContext &context) {
    const auto current = slots_.get(slot_, context);
    if (current.error) {
        throw std::bad_alloc();
    }
    if (current.value) {
        return *static_cast<ThreadState *>(current.value);
    }
    auto state = std::make_unique<ThreadState>(global_);
    if (slots_.set(slot_, state.get(), context)) {
        throw std::bad_alloc();
    }
    return *state.release();
}

CrtLocale &CrtLocales::current() {
    auto *context = guest::ThreadContext::current();
    if (!context) {
        return global_;
    }
    auto &state = bind(*context);
    return state.mode == 1 ? state.locale : global_;
}

std::int32_t CrtLocales::configure(std::int32_t mode) {
    auto *context = guest::ThreadContext::current();
    if (!context || mode < 0 || mode > 2) {
        return -1;
    }
    auto &state = bind(*context);
    const auto previous = state.mode;
    if (mode) {
        state.mode = mode;
    }
    return previous;
}
} // namespace ambrosia::services
