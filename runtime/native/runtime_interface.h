#pragma once
#include "import_registry.h"
#include <array>
#include <atomic>
#include <span>

namespace ambrosia::services {
struct Guid {
    std::uint32_t first;
    std::uint16_t second;
    std::uint16_t third;
    std::array<std::uint8_t, 8> tail;
    bool operator==(const Guid &) const = default;
};

static_assert(sizeof(Guid) == 16);
inline constexpr Guid unknown_interface_id{0, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
inline constexpr std::int32_t no_interface = static_cast<std::int32_t>(0x80004002u);
inline constexpr std::int32_t pointer_error = static_cast<std::int32_t>(0x80004003u);

// Manual Windows interface prefix, not a host compiler's C++ virtual layout.
// The runtime owns one pin; guest references never delete the backing object.
struct RuntimeInterface {
    const std::uintptr_t *vtable;
    Guid class_id;
    std::span<const Guid> interface_ids;
    std::atomic<std::uint32_t> references{1};

    RuntimeInterface(const std::uintptr_t *table, Guid identity, std::span<const Guid> interfaces = {})
        : vtable(table), class_id(identity), interface_ids(interfaces) {}
};

static_assert(offsetof(RuntimeInterface, vtable) == 0);

std::int32_t AMBROSIA_GUEST_ABI interface_query(void *self, const void *interface_id, void *output) noexcept;
std::uint32_t AMBROSIA_GUEST_ABI interface_add_reference(void *self) noexcept;
std::uint32_t AMBROSIA_GUEST_ABI interface_release(void *self) noexcept;
} // namespace ambrosia::services
