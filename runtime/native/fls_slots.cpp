#include "fls_slots.h"
#include "memory_services.h"
#include <array>
#include <exception>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ambrosia::services {
namespace {
constexpr std::size_t slot_limit = 128;
constexpr std::size_t context_limit = 64;
} // namespace

struct FlsSlots::State : std::enable_shared_from_this<State> {
    struct Slot {
        FlsCallback callback = nullptr;
        bool allocated = false;
        bool retiring = false;
        std::uint64_t generation = 0;
    };

    struct Values {
        std::array<void *, slot_limit + 1> entries{};
    };

    std::mutex mutex;
    std::array<Slot, slot_limit + 1> slots{};
    std::map<guest::ThreadContext *, Values> contexts;

    Values &bind(guest::ThreadContext &context) {
        if (auto found = contexts.find(&context); found != contexts.end()) {
            return found->second;
        }
        if (contexts.size() >= context_limit) {
            throw std::runtime_error("FLS context budget exhausted");
        }
        auto [found, inserted] = contexts.emplace(&context, Values{});
        (void)inserted;
        try {
            // State is retained until its final guest cleanup hook, avoiding
            // dangling registry references if handles are released earlier.
            const auto self = shared_from_this();
            auto *key = &context;
            context.on_exit([self, key] { self->cleanup(key); });
        } catch (...) {
            contexts.erase(found);
            throw;
        }
        return found->second;
    }

    bool valid(std::uint32_t index) const {
        return index && index <= slot_limit && slots[index].allocated && !slots[index].retiring;
    }

    void cleanup(guest::ThreadContext *context) {
        for (std::size_t index = 1; index <= slot_limit; ++index) {
            FlsCallback callback = nullptr;
            void *value = nullptr;
            std::uint64_t generation = 0;
            {
                const std::lock_guard guard(mutex);
                const auto found = contexts.find(context);
                if (found == contexts.end()) {
                    return;
                }
                value = std::exchange(found->second.entries[index], nullptr);
                if (slots[index].allocated && !slots[index].retiring) {
                    callback = slots[index].callback;
                    generation = slots[index].generation;
                }
            }
            if (callback && value) {
                callback(value);
            }
            {
                const std::lock_guard guard(mutex);
                const auto found = contexts.find(context);
                if (found != contexts.end() && slots[index].generation == generation) {
                    found->second.entries[index] = nullptr;
                }
            }
        }
        const std::lock_guard guard(mutex);
        contexts.erase(context);
    }
};

FlsSlots::FlsSlots() : state_(std::make_shared<State>()) {}

FlsAllocation FlsSlots::allocate(FlsCallback callback) noexcept {
    try {
        const std::lock_guard guard(state_->mutex);
        for (std::uint32_t index = 1; index <= slot_limit; ++index) {
            auto &slot = state_->slots[index];
            if (!slot.allocated && !slot.retiring) {
                slot.allocated = true;
                slot.callback = callback;
                ++slot.generation;
                return {index, 0};
            }
        }
        return {fls_out_of_indexes, error_not_enough_memory};
    } catch (...) {
        return {fls_out_of_indexes, error_not_enough_memory};
    }
}

FlsValue FlsSlots::get(std::uint32_t index, guest::ThreadContext &context) noexcept {
    try {
        const std::lock_guard guard(state_->mutex);
        if (!state_->valid(index)) {
            return {nullptr, error_invalid_parameter};
        }
        return {state_->bind(context).entries[index], 0};
    } catch (...) {
        return {nullptr, error_not_enough_memory};
    }
}

std::uint32_t FlsSlots::set(std::uint32_t index, void *value, guest::ThreadContext &context) noexcept {
    try {
        const std::lock_guard guard(state_->mutex);
        if (!state_->valid(index)) {
            return error_invalid_parameter;
        }
        state_->bind(context).entries[index] = value;
        return 0;
    } catch (...) {
        return error_not_enough_memory;
    }
}

std::uint32_t FlsSlots::free(std::uint32_t index) {
    FlsCallback callback = nullptr;
    std::vector<void *> pending;
    {
        const std::lock_guard guard(state_->mutex);
        if (!state_->valid(index)) {
            return error_invalid_parameter;
        }
        pending.reserve(state_->contexts.size());
        auto &slot = state_->slots[index];
        slot.retiring = true;
        callback = slot.callback;
        for (auto &[context, values] : state_->contexts) {
            (void)context;
            if (auto *value = std::exchange(values.entries[index], nullptr)) {
                pending.push_back(value);
            }
        }
    }
    if (callback) {
        for (auto *value : pending) {
            try {
                callback(value);
            } catch (...) {
                std::terminate();
            }
        }
    }
    {
        const std::lock_guard guard(state_->mutex);
        auto &slot = state_->slots[index];
        slot.callback = nullptr;
        slot.allocated = false;
        slot.retiring = false;
    }
    return 0;
}
} // namespace ambrosia::services
