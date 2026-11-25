#include "runtime_interface.h"
#include "startup_trace.h"
#include <algorithm>
#include <cstring>

namespace ambrosia::services {
std::uint32_t AMBROSIA_GUEST_ABI interface_add_reference(void *self) noexcept {
    if (!self) {
        diagnostics::stop_trace("native interface AddRef requires an object", 78);
    }
    auto &references = static_cast<RuntimeInterface *>(self)->references;
    auto previous = references.load();
    for (;;) {
        if (!previous || previous >= 4096) {
            diagnostics::stop_trace("native interface reference budget exceeded", 78);
        }
        if (references.compare_exchange_weak(previous, previous + 1)) {
            return previous + 1;
        }
    }
}

std::uint32_t AMBROSIA_GUEST_ABI interface_release(void *self) noexcept {
    if (!self) {
        diagnostics::stop_trace("native interface Release requires an object", 78);
    }
    auto &references = static_cast<RuntimeInterface *>(self)->references;
    auto previous = references.load();
    while (previous > 1) {
        if (references.compare_exchange_weak(previous, previous - 1)) {
            return previous - 1;
        }
    }
    return 1;
}

std::int32_t AMBROSIA_GUEST_ABI interface_query(void *self, const void *interface_id, void *output) noexcept {
    if (!output) {
        return pointer_error;
    }
    void *result = nullptr;
    std::memcpy(output, &result, sizeof(result));
    if (!self || !interface_id) {
        return pointer_error;
    }
    const auto &object = *static_cast<RuntimeInterface *>(self);
    Guid requested;
    std::memcpy(&requested, interface_id, sizeof(requested));
    const bool supported = requested == unknown_interface_id || requested == object.class_id ||
                           std::find(object.interface_ids.begin(), object.interface_ids.end(), requested) !=
                               object.interface_ids.end();
    if (!supported) {
        return no_interface;
    }
    (void)interface_add_reference(self);
    result = self;
    std::memcpy(output, &result, sizeof(result));
    return 0;
}
} // namespace ambrosia::services
