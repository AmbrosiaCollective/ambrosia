#include "xmem_services.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

namespace ambrosia::services {
namespace {
struct PreserveNativeError {
    int saved = errno;

    ~PreserveNativeError() {
        errno = saved;
    }
};

void trace_physical_allocation(std::uint64_t attributes, std::uint64_t count, std::uint32_t error) noexcept {
    char message[192];
    const auto length = std::snprintf(
        message, sizeof(message), "TRACE XMem physical pages=%llu attributes=%llx error=%u\n",
        static_cast<unsigned long long>(count), static_cast<unsigned long long>(attributes), error);
    if (length > 0 && static_cast<std::size_t>(length) < sizeof(message)) {
        diagnostics::write_service_trace(std::string_view(message, static_cast<std::size_t>(length)));
    }
}

std::int32_t AMBROSIA_GUEST_ABI allocate_physical(std::uint64_t attributes, void *page_count,
                                                  void *page_array) noexcept {
    PreserveNativeError restore;
    auto *runtime = current_runtime();
    if (!runtime) {
        diagnostics::stop_trace("XMem physical allocation requires a scoped runtime", 78);
    }
    std::uint64_t count = 0;
    if (page_count) {
        std::memcpy(&count, page_count, sizeof(count));
    }
    if (!page_count || (count && !page_array)) {
        set_last_error(error_invalid_parameter);
        return 0;
    }
    auto allocation = runtime->physical_pages().allocate(attributes, count);
    trace_physical_allocation(attributes, count, allocation.error);
    if (allocation.error == error_not_supported) {
        diagnostics::stop_trace("unsupported XMem physical attributes or missing backing configuration", 78);
    }
    if (allocation.error) {
        const std::uint64_t zero = 0;
        std::memcpy(page_count, &zero, sizeof(zero));
        set_last_error(allocation.error);
        return 0;
    }
    if (count) {
        std::memcpy(page_array, allocation.pages.data(), allocation.pages.size() * sizeof(std::uint64_t));
    }
    diagnostics::write_service_trace("TRACE native XMem physical pages allocated\n");
    return 1;
}

std::int32_t AMBROSIA_GUEST_ABI free_physical(std::uint64_t count, const void *page_array) noexcept {
    PreserveNativeError restore;
    auto *runtime = current_runtime();
    if (!runtime) {
        diagnostics::stop_trace("XMem physical release requires a scoped runtime", 78);
    }
    try {
        if ((count && !page_array) || count > runtime->physical_pages().capacity()) {
            set_last_error(error_invalid_parameter);
            return 0;
        }
        std::vector<std::uint64_t> tokens(static_cast<std::size_t>(count));
        if (count) {
            std::memcpy(tokens.data(), page_array, tokens.size() * sizeof(std::uint64_t));
        }
        const auto error = runtime->physical_pages().free(tokens);
        if (error) {
            set_last_error(error);
            return 0;
        }
        diagnostics::write_service_trace("TRACE native XMem physical pages released\n");
        return 1;
    } catch (...) {
        set_last_error(error_not_enough_memory);
        return 0;
    }
}

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
    if (result.error == error_not_supported) {
        const auto region = runtime->memory().xmem_region(address);
        char target[192];
        const auto length = std::snprintf(
            target, sizeof(target),
            "TRACE XMem target address_present=%u tracked=%u backing=%u guest_page=%llu region_bytes=%llu\n",
            address ? 1u : 0u, region ? 1u : 0u,
            region ? (region->backing == XmemBacking::physical_window ? 2u : 1u) : 0u,
            static_cast<unsigned long long>(region ? region->guest_page_bytes : 0),
            static_cast<unsigned long long>(region ? region->bytes : 0));
        if (length > 0 && static_cast<std::size_t>(length) < sizeof(target)) {
            diagnostics::write_service_trace(std::string_view(target, static_cast<std::size_t>(length)));
        }
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
        if (result.error == error_not_supported && allocation_type == 0x20401000u && attributes == 0x283 &&
            protection == 0x42404) {
            diagnostics::stop_trace("unsupported XMem virtual allocation profile", 78);
        }
    }
    return result.address;
}
} // namespace

void register_xmem_services(ImportRegistry &registry) {
    registry.add("xmem.dll", "XMemAllocatePhysicalPages",
                 reinterpret_cast<std::uintptr_t>(&allocate_physical));
    registry.add("xmem.dll", "XMemFreePhysicalPages", reinterpret_cast<std::uintptr_t>(&free_physical));
    registry.add("xmem.dll", "XMemVirtualAlloc", reinterpret_cast<std::uintptr_t>(&virtual_allocate));
}
} // namespace ambrosia::services
