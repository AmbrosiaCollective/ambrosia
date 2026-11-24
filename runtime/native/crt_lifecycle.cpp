#include "crt_lifecycle.h"
#include <cstring>
#include <utility>

namespace ambrosia::services {
namespace {
constexpr std::size_t table_limit = 128;
constexpr std::size_t per_table_limit = 4096;
constexpr std::size_t callback_limit = 65536;

bool valid_address(void *table) {
    return table && reinterpret_cast<std::uintptr_t>(table) % 8 == 0;
}
} // namespace

OnExitTable OnExitTables::view(const State &state) {
    if (state.callbacks.empty()) {
        return {};
    }
    const auto first = reinterpret_cast<std::uintptr_t>(state.callbacks.data());
    return {first, first + state.callbacks.size() * sizeof(OnExitCallback),
            first + state.callbacks.capacity() * sizeof(OnExitCallback)};
}

bool OnExitTables::matches(void *table, const State &state) {
    OnExitTable guest;
    std::memcpy(&guest, table, sizeof(guest));
    const auto expected = view(state);
    return guest.first == expected.first && guest.last == expected.last && guest.end == expected.end;
}

void OnExitTables::publish(void *table, const State &state) {
    const auto guest = view(state);
    std::memcpy(table, &guest, sizeof(guest));
}

std::int32_t OnExitTables::initialize(void *table) noexcept {
    if (!valid_address(table)) {
        return -1;
    }
    try {
        const std::lock_guard guard(mutex_);
        if (const auto found = tables_.find(table); found != tables_.end()) {
            return matches(table, found->second) ? 0 : -1;
        }
        OnExitTable guest;
        std::memcpy(&guest, table, sizeof(guest));
        if (guest.first != guest.end || tables_.size() >= table_limit) {
            return -1;
        }
        auto [found, inserted] = tables_.try_emplace(table);
        (void)inserted;
        publish(table, found->second);
        return 0;
    } catch (...) {
        return -1;
    }
}

std::int32_t OnExitTables::add(void *table, OnExitCallback callback) noexcept {
    if (initialize(table)) {
        return -1;
    }
    try {
        const std::lock_guard guard(mutex_);
        auto &state = tables_.at(table);
        if (!matches(table, state) || state.callbacks.size() >= per_table_limit ||
            callbacks_ >= callback_limit) {
            return -1;
        }
        state.callbacks.push_back(callback);
        ++callbacks_;
        publish(table, state);
        return 0;
    } catch (...) {
        return -1;
    }
}

std::int32_t OnExitTables::execute(void *table) {
    if (initialize(table)) {
        return -1;
    }
    std::vector<OnExitCallback> pending;
    {
        const std::lock_guard guard(mutex_);
        auto &state = tables_.at(table);
        if (!matches(table, state) || state.depth >= 32) {
            return -1;
        }
        pending.swap(state.callbacks);
        ++state.depth;
        // Clear the guest view before calling user code. Registrations made by
        // a callback form a fresh batch for a later explicit execution.
        publish(table, state);
    }
    auto finish = [&] {
        const std::lock_guard guard(mutex_);
        --tables_.at(table).depth;
        callbacks_ -= pending.size();
    };
    try {
        for (auto callback = pending.rbegin(); callback != pending.rend(); ++callback) {
            if (*callback) {
                (void)(*callback)();
            }
        }
    } catch (...) {
        finish();
        throw;
    }
    finish();
    return 0;
}
} // namespace ambrosia::services
