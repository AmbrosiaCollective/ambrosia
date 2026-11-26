#include "crt_heap.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include <cstring>
#include <limits>

namespace ambrosia::services {
namespace {
Runtime &runtime() noexcept {
    auto *runtime = current_runtime();
    if (!runtime) {
        diagnostics::stop_trace("CRT heap accessed without scoped runtime", 78);
    }
    return *runtime;
}

CrtHeap &heap() noexcept {
    return runtime().heap();
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

bool invoke_new_handler(std::size_t bytes) noexcept {
    const auto handler = runtime().new_allocations().handler();
    if (!handler) {
        return false;
    }
    try {
        return handler(bytes) != 0;
    } catch (...) {
        diagnostics::stop_trace("guest new-handler exception propagation is not implemented", 78);
    }
}

template <class Operation> void *retry_allocation(std::size_t bytes, Operation operation) noexcept {
    for (;;) {
        const auto result = operation();
        // The heap operation has released its lock before guest code runs. A
        // handler may free storage, allocate recursively or replace itself.
        if (result.error != HeapError::exhausted || runtime().new_allocations().mode() == 0 ||
            !invoke_new_handler(bytes)) {
            return allocation_result(result);
        }
    }
}

constexpr std::size_t maximum_crt_request = SIZE_MAX - 31;

void *AMBROSIA_GUEST_ABI allocate(std::size_t bytes) noexcept {
    if (bytes > maximum_crt_request) {
        return allocation_result({nullptr, HeapError::exhausted});
    }
    const auto actual = bytes ? bytes : 1;
    return retry_allocation(actual, [&] { return heap().allocate(actual); });
}

void *AMBROSIA_GUEST_ABI allocate_zero(std::size_t count, std::size_t bytes) noexcept {
    if (count && bytes > maximum_crt_request / count) {
        crt_error() = 12;
        return nullptr;
    }
    const auto actual = count && bytes ? count * bytes : 1;
    return retry_allocation(actual, [&] { return heap().allocate(actual, true); });
}

void *AMBROSIA_GUEST_ABI resize(void *address, std::size_t bytes) noexcept {
    if (!address) {
        return allocate(bytes);
    }
    if (bytes > maximum_crt_request) {
        return allocation_result({nullptr, HeapError::exhausted});
    }
    return retry_allocation(bytes, [&] { return heap().resize(address, bytes); });
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

std::int32_t AMBROSIA_GUEST_ABI set_new_mode(std::int32_t mode) noexcept {
    const auto previous = runtime().new_allocations().replace_mode(mode);
    if (previous < 0) {
        crt_error() = 22;
        dispatch_invalid_parameter();
        return -1;
    }
    diagnostics::write_service_trace("TRACE CRT allocation-failure mode accepted\n");
    return previous;
}

std::int32_t AMBROSIA_GUEST_ABI query_new_mode() noexcept {
    return runtime().new_allocations().mode();
}

NewAllocationHandler AMBROSIA_GUEST_ABI set_new_handler(NewAllocationHandler handler) noexcept {
    return runtime().new_allocations().replace_handler(handler);
}

NewAllocationHandler AMBROSIA_GUEST_ABI query_new_handler() noexcept {
    return runtime().new_allocations().handler();
}

std::int32_t AMBROSIA_GUEST_ABI call_new_handler(std::size_t bytes) noexcept {
    return invoke_new_handler(bytes) ? 1 : 0;
}
} // namespace

void register_crt_heap(ImportRegistry &registry) {
    for (const auto *module : {"ucrtbase.dll", "msvcrt.dll", "api-ms-win-crt-heap-l1-1-0.dll"}) {
        registry.add(module, "_set_new_mode", reinterpret_cast<std::uintptr_t>(&set_new_mode));
        registry.add(module, "_query_new_mode", reinterpret_cast<std::uintptr_t>(&query_new_mode));
        registry.add(module, "_set_new_handler", reinterpret_cast<std::uintptr_t>(&set_new_handler));
        registry.add(module, "_query_new_handler", reinterpret_cast<std::uintptr_t>(&query_new_handler));
        registry.add(module, "_callnewh", reinterpret_cast<std::uintptr_t>(&call_new_handler));
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
