#pragma once
#include "pe_image.h"

namespace ambrosia::services {
using FlsCallback = void(AMBROSIA_GUEST_ABI *)(void *);
inline constexpr std::uint32_t fls_out_of_indexes = 0xffffffffu;

struct FlsAllocation {
    std::uint32_t index = fls_out_of_indexes;
    std::uint32_t error = 0;
};

struct FlsValue {
    void *value = nullptr;
    std::uint32_t error = 0;
};

class FlsSlots {
  public:
    FlsSlots();
    FlsSlots(const FlsSlots &) = delete;
    FlsSlots &operator=(const FlsSlots &) = delete;
    FlsAllocation allocate(FlsCallback callback) noexcept;
    FlsValue get(std::uint32_t index, guest::ThreadContext &context) noexcept;
    std::uint32_t set(std::uint32_t index, void *value, guest::ThreadContext &context) noexcept;
    std::uint32_t free(std::uint32_t index);

  private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace ambrosia::services
