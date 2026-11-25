#pragma once
#include "crt_locale.h"
#include "fls_slots.h"

namespace ambrosia::services {
class CrtLocales {
  public:
    CrtLocales();
    CrtLocale &current();
    std::int32_t configure(std::int32_t mode);

  private:
    struct ThreadState;
    ThreadState &bind(guest::ThreadContext &context);
    static void AMBROSIA_GUEST_ABI destroy_thread_state(void *value);
    CrtLocale global_;
    // This private registry does not consume indices in the guest FLS service.
    // Its existing retained cleanup hook owns state through context teardown.
    FlsSlots slots_;
    std::uint32_t slot_;
};
} // namespace ambrosia::services
