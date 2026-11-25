#include "xmem_services.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include <cstdio>

namespace ambrosia::services {
namespace {
void *AMBROSIA_GUEST_ABI virtual_allocate(void *address, std::size_t bytes, std::uint32_t allocation_type,
                                          std::uint64_t attributes, std::uint32_t protection) noexcept {
    auto *runtime = current_runtime();
    if (!runtime) {
        diagnostics::stop_trace("XMem reservation requires a scoped runtime", 78);
    }
    // These are the inherited measured reserve-only 64KiB window profiles.
    // Attribute/GPU/cache semantics are not guessed: retain the raw policy for
    // physical-page mapping, which must establish actual backing separately.
    const bool known_attributes = attributes == 3 || attributes == 0x21 || attributes == 0x23;
    const bool known_protection =
        protection == 4 || protection == 0x801 || protection == 0x2004 || protection == 0x2404;
    AllocationResult result;
    if (address || allocation_type != 0x20402000u || !known_attributes || !known_protection) {
        result.error = error_not_supported;
    } else {
        result = runtime->memory().reserve_window(bytes, attributes, protection);
    }
    // Bounded numeric request evidence stays in explicit private trace runs.
    // No guest pointer, input bytes, host address or file path is disclosed.
    char message[192];
    const auto length =
        std::snprintf(message, sizeof(message),
                      "TRACE XMem reservation bytes=%llu type=%x attributes=%llx protection=%x error=%u\n",
                      static_cast<unsigned long long>(bytes), allocation_type,
                      static_cast<unsigned long long>(attributes), protection, result.error);
    if (length > 0 && static_cast<std::size_t>(length) < sizeof(message)) {
        diagnostics::write_service_trace(std::string_view(message, static_cast<std::size_t>(length)));
    }
    if (result.error) {
        set_last_error(result.error);
    }
    return result.address;
}
} // namespace

void register_xmem_services(ImportRegistry &registry) {
    registry.add("xmem.dll", "XMemVirtualAlloc", reinterpret_cast<std::uintptr_t>(&virtual_allocate));
}
} // namespace ambrosia::services
