#include "crt_heap.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include <cstring>
#include <limits>

namespace ambrosia::services {
namespace {
CrtHeap &heap() noexcept {
    auto *runtime = current_runtime();
    if (!runtime) {
        diagnostics::stop_trace("CRT heap accessed without scoped runtime", 78);
    }
    return runtime->heap();
}

void *allocation_result(HeapResult result) noexcept {
    if (result.error == HeapError::foreign_pointer) {
        diagnostics::stop_trace("CRT heap received foreign allocation", 78);
    }
    if (result.error == HeapError::exhausted) {
        crt_error() = 12; // ENOMEM in the guest CRT ABI, independent of host errno.
    }
    return result.address;
}

void *AMBROSIA_GUEST_ABI allocate(std::size_t bytes) noexcept {
    return allocation_result(heap().allocate(bytes));
}

void *AMBROSIA_GUEST_ABI allocate_zero(std::size_t count, std::size_t bytes) noexcept {
    if (bytes && count > std::numeric_limits<std::size_t>::max() / bytes) {
        crt_error() = 12;
        return nullptr;
    }
    return allocation_result(heap().allocate(count * bytes, true));
}

void *AMBROSIA_GUEST_ABI resize(void *address, std::size_t bytes) noexcept {
    return allocation_result(heap().resize(address, bytes));
}

void AMBROSIA_GUEST_ABI release(void *address) noexcept {
    if (!heap().release(address)) {
        diagnostics::stop_trace("CRT free received foreign allocation", 78);
    }
}

std::int32_t *AMBROSIA_GUEST_ABI error_address() noexcept {
    return &crt_error();
}

std::int32_t AMBROSIA_GUEST_ABI get_error(void *output) noexcept {
    if (!output) {
        return 22; // EINVAL.
    }
    const auto error = crt_error();
    std::memcpy(output, &error, sizeof(error));
    return 0;
}

std::int32_t AMBROSIA_GUEST_ABI set_error(std::int32_t error) noexcept {
    crt_error() = error;
    return 0;
}
} // namespace

void register_crt_heap(ImportRegistry &registry) {
    for (const auto *module : {"ucrtbase.dll", "msvcrt.dll", "api-ms-win-crt-heap-l1-1-0.dll"}) {
        for (const auto *name : {"malloc", "_malloc_base"}) {
            registry.add(module, name, reinterpret_cast<std::uintptr_t>(&allocate));
        }
        for (const auto *name : {"calloc", "_calloc_base"}) {
            registry.add(module, name, reinterpret_cast<std::uintptr_t>(&allocate_zero));
        }
        for (const auto *name : {"realloc", "_realloc_base"}) {
            registry.add(module, name, reinterpret_cast<std::uintptr_t>(&resize));
        }
        for (const auto *name : {"free", "_free_base"}) {
            registry.add(module, name, reinterpret_cast<std::uintptr_t>(&release));
        }
    }
    for (const auto *module : {"ucrtbase.dll", "msvcrt.dll", "api-ms-win-crt-runtime-l1-1-0.dll"}) {
        registry.add(module, "_errno", reinterpret_cast<std::uintptr_t>(&error_address));
        registry.add(module, "_get_errno", reinterpret_cast<std::uintptr_t>(&get_error));
        registry.add(module, "_set_errno", reinterpret_cast<std::uintptr_t>(&set_error));
    }
}
} // namespace ambrosia::services
