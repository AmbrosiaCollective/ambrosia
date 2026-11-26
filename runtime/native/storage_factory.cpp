#include "storage_factory.h"
#include "gdk_bootstrap.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include <cerrno>
#include <stdexcept>
#include <string>

namespace ambrosia::services {
namespace {
template <unsigned Slot> [[noreturn]] void AMBROSIA_GUEST_ABI unsupported() noexcept {
    diagnostics::stop_trace("unsupported console DirectStorage factory slot " + std::to_string(Slot), 78);
}

struct NativeErrorScope {
    int saved = errno;

    ~NativeErrorScope() {
        errno = saved;
    }
};

StorageFactory &factory() {
    auto *owner = current_runtime();
    if (!owner) {
        diagnostics::stop_trace("console storage requires a scoped native runtime", 78);
    }
    return owner->storage_factory();
}

StorageFactory &method_factory(void *self) {
    auto &owner = factory();
    if (!owner.owns_interface(self)) {
        diagnostics::stop_trace("storage factory method belongs to another scoped runtime", 78);
    }
    return owner;
}

std::int32_t AMBROSIA_GUEST_ABI get_factory(const void *interface_id, void *output) noexcept {
    NativeErrorScope restore;
    return factory().query(interface_id, output);
}

std::int32_t AMBROSIA_GUEST_ABI staging_size(void *self, std::uint32_t bytes) noexcept {
    NativeErrorScope restore;
    try {
        return method_factory(self).set_staging_size(bytes);
    } catch (const std::bad_alloc &) {
        return gdk_out_of_memory;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

void AMBROSIA_GUEST_ABI debug_flags(void *self, std::uint32_t flags) noexcept {
    NativeErrorScope restore;
    try {
        method_factory(self).set_debug_flags(flags);
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

std::int32_t AMBROSIA_GUEST_ABI cpu_affinity(void *self, std::uint64_t mask) noexcept {
    NativeErrorScope restore;
    (void)method_factory(self);
    (void)mask;
    // The factory has no I/O worker scheduler yet; do not claim to have
    // applied an affinity policy to the caller or to nonexistent workers.
    return static_cast<std::int32_t>(0x80070032u);
}

const std::array<std::uintptr_t, 16> factory_vtable{
    reinterpret_cast<std::uintptr_t>(&interface_query),
    reinterpret_cast<std::uintptr_t>(&interface_add_reference),
    reinterpret_cast<std::uintptr_t>(&interface_release),
    reinterpret_cast<std::uintptr_t>(&unsupported<3>),
    reinterpret_cast<std::uintptr_t>(&unsupported<4>),
    reinterpret_cast<std::uintptr_t>(&unsupported<5>),
    reinterpret_cast<std::uintptr_t>(&cpu_affinity),
    reinterpret_cast<std::uintptr_t>(&debug_flags),
    reinterpret_cast<std::uintptr_t>(&staging_size),
    reinterpret_cast<std::uintptr_t>(&unsupported<9>),
    reinterpret_cast<std::uintptr_t>(&unsupported<10>),
    reinterpret_cast<std::uintptr_t>(&unsupported<11>),
    reinterpret_cast<std::uintptr_t>(&unsupported<12>),
    reinterpret_cast<std::uintptr_t>(&unsupported<13>),
    reinterpret_cast<std::uintptr_t>(&unsupported<14>),
    reinterpret_cast<std::uintptr_t>(&unsupported<15>)};
} // namespace

StorageFactory::StorageFactory() : object_(factory_vtable.data(), storage_factory_x2_id) {}

std::int32_t StorageFactory::query(const void *interface_id, void *output) noexcept {
    return interface_query(&object_, interface_id, output);
}

std::int32_t StorageFactory::set_staging_size(std::uint32_t bytes) {
    if (bytes > staging_limit) {
        return gdk_out_of_memory;
    }
    // Allocate before publishing any change. There are no request queues yet;
    // later execution must retain staging ownership before making work visible.
    std::vector<std::byte> replacement(bytes);
    const std::lock_guard lock(mutex_);
    staging_.swap(replacement);
    return 0;
}

void StorageFactory::set_debug_flags(std::uint32_t flags) {
    if (flags & ~7u) {
        diagnostics::stop_trace("unknown console storage debug policy flags", 78);
    }
    const std::lock_guard lock(mutex_);
    debug_flags_ = flags;
}

StorageConfiguration StorageFactory::configuration() const {
    const std::lock_guard lock(mutex_);
    return {static_cast<std::uint32_t>(staging_.size()), debug_flags_};
}

void register_storage_factory(ImportRegistry &registry) {
    registry.add("xcrdapi.dll", "DStorageGetFactory", reinterpret_cast<std::uintptr_t>(&get_factory));
}
} // namespace ambrosia::services
