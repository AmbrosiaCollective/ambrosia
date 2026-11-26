#include "storage_factory.h"
#include "gdk_bootstrap.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include "text_conversion.h"
#include <cerrno>
#include <cstring>
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

std::int32_t AMBROSIA_GUEST_ABI open_file(void *self, const void *path, const void *interface_id,
                                          void *output) noexcept {
    NativeErrorScope restore;
    if (!output) {
        return pointer_error;
    }
    void *empty = nullptr;
    std::memcpy(output, &empty, sizeof(empty));
    if (!path || !interface_id) {
        return pointer_error;
    }
    try {
        std::u16string wide;
        const auto *bytes = static_cast<const std::byte *>(path);
        for (std::size_t index = 0; index < 32768; ++index) {
            char16_t unit;
            std::memcpy(&unit, bytes + index * sizeof(unit), sizeof(unit));
            if (!unit) {
                const auto utf8 = unicode::utf16_to_utf8(wide, true);
                if (!utf8) {
                    return static_cast<std::int32_t>(0x8007007bu);
                }
                auto &owner = method_factory(self);
                return owner.open_file(current_runtime()->files(), *utf8, interface_id, output);
            }
            wide.push_back(unit);
        }
        return static_cast<std::int32_t>(0x800700ceu);
    } catch (const std::bad_alloc &) {
        return gdk_out_of_memory;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

std::int32_t AMBROSIA_GUEST_ABI file_query(void *self, const void *interface_id, void *output) noexcept {
    NativeErrorScope restore;
    return factory().query_file(self, interface_id, output);
}

std::uint32_t AMBROSIA_GUEST_ABI file_add_reference(void *self) noexcept {
    NativeErrorScope restore;
    return factory().add_file_reference(self);
}

std::uint32_t AMBROSIA_GUEST_ABI file_release(void *self) noexcept {
    NativeErrorScope restore;
    return factory().release_file(self);
}

void AMBROSIA_GUEST_ABI file_close(void *self) noexcept {
    NativeErrorScope restore;
    factory().close_file(self);
}

template <unsigned Slot> [[noreturn]] void AMBROSIA_GUEST_ABI unsupported_file() noexcept {
    diagnostics::stop_trace("unsupported console DirectStorage file slot " + std::to_string(Slot), 78);
}

const std::array<std::uintptr_t, 8> file_vtable{reinterpret_cast<std::uintptr_t>(&file_query),
                                                reinterpret_cast<std::uintptr_t>(&file_add_reference),
                                                reinterpret_cast<std::uintptr_t>(&file_release),
                                                reinterpret_cast<std::uintptr_t>(&file_close),
                                                reinterpret_cast<std::uintptr_t>(&unsupported_file<4>),
                                                reinterpret_cast<std::uintptr_t>(&unsupported_file<5>),
                                                reinterpret_cast<std::uintptr_t>(&unsupported_file<6>),
                                                reinterpret_cast<std::uintptr_t>(&unsupported_file<7>)};

const std::array<std::uintptr_t, 16> factory_vtable{
    reinterpret_cast<std::uintptr_t>(&interface_query),
    reinterpret_cast<std::uintptr_t>(&interface_add_reference),
    reinterpret_cast<std::uintptr_t>(&interface_release),
    reinterpret_cast<std::uintptr_t>(&unsupported<3>),
    reinterpret_cast<std::uintptr_t>(&open_file),
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

struct StorageFactory::File {
    RuntimeInterface object{file_vtable.data(), storage_file_x_id};
    mutable std::mutex mutex;
    std::unique_ptr<host::ReadonlyFile> descriptor;
};

StorageFactory::StorageFactory() : object_(factory_vtable.data(), storage_factory_x2_id) {}

StorageFactory::~StorageFactory() = default;

StorageFactory::File &StorageFactory::file(void *self) const {
    const std::lock_guard lock(mutex_);
    for (const auto &entry : files_) {
        if (&entry->object == self) {
            return *entry;
        }
    }
    diagnostics::stop_trace("storage file belongs to another scoped runtime", 78);
}

std::int32_t StorageFactory::open_file(ReadonlyFiles &files, std::string_view path, const void *interface_id,
                                       void *output) {
    if (!output) {
        return pointer_error;
    }
    void *empty = nullptr;
    std::memcpy(output, &empty, sizeof(empty));
    if (!interface_id) {
        return pointer_error;
    }
    Guid requested;
    std::memcpy(&requested, interface_id, sizeof(requested));
    if (requested != storage_file_x_id && requested != unknown_interface_id) {
        return no_interface;
    }
    const std::lock_guard lock(mutex_);
    if (files_.size() >= 1024) {
        return gdk_out_of_memory;
    }
    unsigned active = 0;
    for (const auto &entry : files_) {
        const std::lock_guard file_lock(entry->mutex);
        if (entry->descriptor) {
            ++active;
        }
    }
    if (active >= 32) {
        return gdk_out_of_memory;
    }
    auto opened = files.open(path);
    if (opened.error) {
        return static_cast<std::int32_t>(0x80070000u | opened.error);
    }
    auto entry = std::make_unique<File>();
    entry->descriptor = std::move(opened.file);
    auto *object = &entry->object;
    files_.push_back(std::move(entry));
    const auto result = interface_query(object, interface_id, output);
    diagnostics::write_service_trace("TRACE native storage read-only file opened\n");
    return result;
}

std::int32_t StorageFactory::query_file(void *self, const void *interface_id, void *output) {
    auto &entry = file(self);
    const std::lock_guard lock(entry.mutex);
    if (entry.object.references.load() == 1) {
        diagnostics::stop_trace("querying a released storage file", 78);
    }
    return interface_query(&entry.object, interface_id, output);
}

std::uint32_t StorageFactory::add_file_reference(void *self) {
    auto &entry = file(self);
    const std::lock_guard lock(entry.mutex);
    if (entry.object.references.load() == 1) {
        diagnostics::stop_trace("retaining a released storage file", 78);
    }
    return interface_add_reference(&entry.object);
}

std::uint32_t StorageFactory::release_file(void *self) {
    auto &entry = file(self);
    const std::lock_guard lock(entry.mutex);
    const auto count = interface_release(&entry.object);
    if (count == 1) {
        entry.descriptor.reset();
    }
    return count;
}

void StorageFactory::close_file(void *self) {
    auto &entry = file(self);
    const std::lock_guard lock(entry.mutex);
    entry.descriptor.reset();
}

host::FileLookup StorageFactory::inspect_file(void *self) const {
    auto &entry = file(self);
    const std::lock_guard lock(entry.mutex);
    if (!entry.descriptor) {
        return {{}, host::FileLookupError::invalid};
    }
    return entry.descriptor->inspect();
}

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
