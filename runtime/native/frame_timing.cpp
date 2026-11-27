// Copyright (c) 2026 Five Star Recomp contributors.
// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted timer-grid policy; see REUSE.md for the pinned source and native changes.
#include "frame_timing.h"
#include <limits>

namespace ambrosia::services {
namespace {
constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint32_t infinite_timeout = 0xffffffffu;
constexpr std::uint32_t current_origin_flag = 2;

bool add(std::uint64_t left, std::uint64_t right, std::uint64_t &output) {
    if (left > maximum - right) {
        return false;
    }
    output = left + right;
    return true;
}
} // namespace

bool FrameTiming::set_interval(std::uint32_t period_us, std::uint32_t latency, std::uint64_t now_us) {
    if (!period_us || period_us > 1000000 || !latency || latency > 16) {
        return false;
    }
    if (configuration_.period_us != period_us || configuration_.latency != latency) {
        configuration_ = {period_us, latency, now_us};
    }
    return true;
}

bool FrameTiming::schedule(std::uint32_t event, std::uint32_t offset_us) {
    if (!configuration_.period_us || event >= offsets_.size() || offset_us > 1000000) {
        return false;
    }
    offsets_[event] = offset_us;
    scheduled_[event] = true;
    return true;
}

FrameWait FrameTiming::prepare_wait(std::uint32_t event, std::uint32_t timeout_ms, std::uint32_t flags,
                                    std::uint64_t token, std::uint64_t now_us) const {
    if (!configuration_.period_us) {
        return {static_cast<std::int32_t>(0x8007139fu), 0, 0};
    }
    if (event >= offsets_.size() || (event != 0 && !scheduled_[event]) || (flags & ~current_origin_flag) ||
        (event != 0 && flags)) {
        return {};
    }
    std::uint64_t first;
    if (!add(configuration_.epoch_us, offsets_[event], first)) {
        return {};
    }
    std::uint64_t reached = 0;
    if (now_us >= first) {
        const auto completed = (now_us - first) / configuration_.period_us;
        if (!add(completed, 1, reached)) {
            return {};
        }
    }
    if (event == 0 && ((flags & current_origin_flag) || timeout_ms == 0)) {
        // This is a timer-origin token, not proof of a presented/completed frame.
        return {0, now_us, reached ? reached : 1};
    }
    std::uint64_t target;
    if (event != 0 && token) {
        target = token;
    } else if (!add(reached, 1, target)) {
        return {};
    }
    const auto intervals = target - 1;
    if (intervals > maximum / configuration_.period_us) {
        return {};
    }
    std::uint64_t due;
    if (!add(first, intervals * configuration_.period_us, due)) {
        return {};
    }
    if (timeout_ms != infinite_timeout) {
        std::uint64_t deadline;
        if (!add(now_us, static_cast<std::uint64_t>(timeout_ms) * 1000, deadline)) {
            return {};
        }
        if (due > deadline) {
            return {1, deadline, token};
        }
    }
    return {0, due, target};
}

const FrameConfiguration &FrameTiming::configuration() const {
    return configuration_;
}
} // namespace ambrosia::services
