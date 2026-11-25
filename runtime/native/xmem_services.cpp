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
        diagnostics::stop_trace("XMem allocation requires a scoped runtime", 78);
    }
    // Retain inherited measured profile words without inventing attribute/GPU
    // semantics. CPU backing and physical-page windows remain distinct owners.
    const bool known_attributes = attributes == 3 || attributes == 0x21 || attributes == 0x23;
    const bool known_protection =
        protection == 4 || protection == 0x801 || protection == 0x2004 || protection == 0x2404;
    AllocationResult result;
    const bool committed_64k =
        allocation_type == 0x20403000u &&
        ((attributes == 0x181 && protection == 4) || (attributes == 0x283 && protection == 0x42404));
    const bool committed_2m = allocation_type == 0x20003000u &&
                              ((attributes == 0x183 && protection == 0x22004) ||
                               (attributes == 0x283 && (protection == 0x2004 || protection == 0x2404)));
    if (address) {
        result.error = error_not_supported;
    } else if (allocation_type == 0x20402000u && known_attributes && known_protection) {
        result = runtime->memory().reserve_window(bytes, attributes, protection);
    } else if (committed_64k || committed_2m) {
        const auto guest_page = committed_2m ? 2097152 : 65536;
        result = runtime->memory().allocate_xmem(bytes, attributes, protection, guest_page);
    } else {
        result.error = error_not_supported;
    }
    // Bounded numeric request evidence stays in explicit private trace runs.
    // No guest pointer, input bytes, host address or file path is disclosed.
    char message[192];
    const auto length =
        std::snprintf(message, sizeof(message),
                      "TRACE XMem allocation bytes=%llu type=%x attributes=%llx protection=%x error=%u\n",
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
