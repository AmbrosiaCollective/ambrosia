#include "persistent_storage.h"
#include "guest_thread.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include "text_conversion.h"
#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace ambrosia::services {
namespace {
struct RestoreError {
    int saved = errno;

    ~RestoreError() {
        errno = saved;
    }
};

void ordinary_thread() {
    const auto *context = guest::ThreadContext::current();
    if (!context || context->time_sensitive()) {
        throw std::runtime_error("persistent storage requires an ordinary active guest thread");
    }
}

PersistentStorage &storage() {
    auto *runtime = current_runtime();
    if (!runtime) {
        throw std::runtime_error("persistent storage requires a scoped runtime");
    }
    return runtime->persistent_storage();
}

std::int32_t AMBROSIA_GUEST_ABI get_path_size(void *, std::size_t *size) noexcept {
    RestoreError restore;
    try {
        ordinary_thread();
        if (!size) {
            return pointer_error;
        }
        const std::size_t empty = 0;
        std::memcpy(size, &empty, sizeof(empty));
        const auto path = storage().path();
        if (!path) {
            return static_cast<std::int32_t>(0x80070003u);
        }
        const auto needed = path->size() + 1;
        std::memcpy(size, &needed, sizeof(needed));
        return 0;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

std::int32_t AMBROSIA_GUEST_ABI get_path(void *, std::size_t size, char *output, std::size_t *used) noexcept {
    RestoreError restore;
    try {
        ordinary_thread();
        if (used) {
            const std::size_t empty = 0;
            std::memcpy(used, &empty, sizeof(empty));
        }
        if (!output) {
            return pointer_error;
        }
        const auto path = storage().path();
        if (!path) {
            return static_cast<std::int32_t>(0x80070003u);
        }
        const auto needed = path->size() + 1;
        if (size < needed) {
            // This backend is a native PC port. The documented PC contract is
            // HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER), not Xbox E_FAIL.
            return static_cast<std::int32_t>(0x8007007au);
        }
        std::memcpy(output, path->c_str(), needed);
        if (used) {
            std::memcpy(used, &needed, sizeof(needed));
        }
        diagnostics::write_service_trace("TRACE native persistent storage path retrieved\n");
        return 0;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

template <unsigned slot> [[noreturn]] void AMBROSIA_GUEST_ABI unsupported() noexcept {
    diagnostics::stop_trace("unsupported persistent storage slot " + std::to_string(slot), 78);
}

const std::array<Guid, 2> interfaces{persistent_storage_interface_2, persistent_storage_interface_3};
const std::array<std::uintptr_t, 9> vtable{reinterpret_cast<std::uintptr_t>(&interface_query),
                                           reinterpret_cast<std::uintptr_t>(&interface_add_reference),
                                           reinterpret_cast<std::uintptr_t>(&interface_release),
                                           reinterpret_cast<std::uintptr_t>(&get_path_size),
                                           reinterpret_cast<std::uintptr_t>(&get_path),
                                           reinterpret_cast<std::uintptr_t>(&unsupported<5>),
                                           reinterpret_cast<std::uintptr_t>(&unsupported<6>),
                                           reinterpret_cast<std::uintptr_t>(&unsupported<7>),
                                           reinterpret_cast<std::uintptr_t>(&unsupported<8>)};
} // namespace

PersistentStorage::PersistentStorage() : object_(vtable.data(), persistent_storage_class_id, interfaces) {}

void PersistentStorage::bind(const std::filesystem::path &root) {
    const std::lock_guard guard(mutex_);
    if (directory_) {
        throw std::runtime_error("persistent storage directory is already bound");
    }
    directory_ = std::make_shared<const host::LocalStorageDirectory>(root);
}

std::optional<std::string> PersistentStorage::path() const {
    std::shared_ptr<const host::LocalStorageDirectory> directory;
    {
        const std::lock_guard guard(mutex_);
        directory = directory_;
    }
    if (!directory) {
        return std::nullopt;
    }
    const auto utf8 = directory->path().generic_u8string();
    std::string result(utf8.begin(), utf8.end());
    if (result.size() > 32767 || !unicode::utf8_to_utf16(result, true)) {
        throw std::runtime_error("persistent storage path exceeds the guest path contract");
    }
    return result;
}

std::uint32_t PersistentStorage::create_directory(const ResolvedFilePath &path) const {
    if (path.error) {
        return path.error;
    }
    std::shared_ptr<const host::LocalStorageDirectory> directory;
    {
        const std::lock_guard guard(mutex_);
        directory = directory_;
    }
    if (!directory || !path.directory) {
        return 5;
    }
    const auto identity = path.directory->inspect({});
    if (identity.error != host::FileLookupError::none) {
        return 5;
    }
    switch (directory->create_directory(identity.metadata, path.components)) {
    case host::DirectoryCreateError::none:
        return 0;
    case host::DirectoryCreateError::already_exists:
        return 183;
    case host::DirectoryCreateError::missing_parent:
        return 3;
    case host::DirectoryCreateError::denied:
        return 5;
    case host::DirectoryCreateError::invalid:
        return 123;
    case host::DirectoryCreateError::too_long:
        return 206;
    case host::DirectoryCreateError::no_space:
        return 112;
    case host::DirectoryCreateError::exhausted:
        return 8;
    case host::DirectoryCreateError::unsupported:
        return 50;
    case host::DirectoryCreateError::failure:
        return 31;
    }
    return 31;
}

std::int32_t PersistentStorage::query(const Guid &interface_id, void *output) noexcept {
    return interface_query(&object_, &interface_id, output);
}
} // namespace ambrosia::services
