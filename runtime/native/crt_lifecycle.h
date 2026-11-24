#pragma once
#include "import_registry.h"
#include <map>
#include <mutex>

namespace ambrosia::services {
using OnExitCallback = std::int32_t(AMBROSIA_GUEST_ABI *)();
using Initializer = void(AMBROSIA_GUEST_ABI *)();

struct OnExitTable {
    std::uintptr_t first = 0;
    std::uintptr_t last = 0;
    std::uintptr_t end = 0;
};

static_assert(sizeof(OnExitTable) == 24);

class OnExitTables {
  public:
    std::int32_t initialize(void *table) noexcept;
    std::int32_t add(void *table, OnExitCallback callback) noexcept;
    std::int32_t execute(void *table);

  private:
    struct State {
        std::vector<OnExitCallback> callbacks;
        std::size_t depth = 0;
    };

    static OnExitTable view(const State &state);
    static bool matches(void *table, const State &state);
    static void publish(void *table, const State &state);
    std::mutex mutex_;
    std::map<void *, State> tables_;
    std::size_t callbacks_ = 0;
};

void register_crt_lifecycle(ImportRegistry &registry);
} // namespace ambrosia::services
