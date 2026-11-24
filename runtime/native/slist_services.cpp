#include "slist_services.h"
#include "startup_trace.h"
#include <array>
#include <cstring>

namespace ambrosia::services {
namespace {
void AMBROSIA_GUEST_ABI initialize_head(void *head) noexcept {
    if (!head || reinterpret_cast<std::uintptr_t>(head) % 16) {
        diagnostics::stop_trace("invalid x64 sequenced-list head", 78);
    }
    // x64 starts with zero depth, sequence and next-entry fields. Bit zero
    // of the second word selects the 16-byte header representation.
    // Initialization requires exclusive ownership; subsequent interlocked list
    // operations are separate services and remain unresolved for now.
    const std::array<std::uint64_t, 2> empty{0, 1};
    std::memcpy(head, empty.data(), sizeof(empty));
}
} // namespace

void register_slist_services(ImportRegistry &registry) {
    for (const auto *module : {"kernel32.dll", "kernelbase.dll", "api-ms-win-core-interlocked-l1-1-0.dll",
                               "api-ms-win-core-interlocked-l1-2-0.dll"}) {
        registry.add(module, "InitializeSListHead", reinterpret_cast<std::uintptr_t>(&initialize_head));
    }
    registry.add("ntdll.dll", "RtlInitializeSListHead", reinterpret_cast<std::uintptr_t>(&initialize_head));
}
} // namespace ambrosia::services
