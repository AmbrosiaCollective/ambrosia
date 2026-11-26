#include "file_services.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include "text_conversion.h"
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>

namespace ambrosia::services {
namespace {
struct NativeErrorScope {
    int saved = errno;

    ~NativeErrorScope() {
        errno = saved;
    }
};

std::uint32_t failure(std::uint32_t error) noexcept {
    set_last_error(error);
    return 0xffffffffu;
}

ReadonlyFiles &files() {
    auto *owner = current_runtime();
    if (!owner) {
        diagnostics::stop_trace("file metadata requires a scoped native runtime", 78);
    }
    return owner->files();
}

template <class Unit> std::optional<std::basic_string<Unit>> read_filename(const void *source) {
    std::basic_string<Unit> result;
    const auto *bytes = static_cast<const std::byte *>(source);
    constexpr std::size_t limit = sizeof(Unit) == 1 ? 131069 : 32768;
    for (std::size_t index = 0; index < limit; ++index) {
        Unit unit;
        std::memcpy(&unit, bytes + index * sizeof(Unit), sizeof(unit));
        if (!unit) {
            return result;
        }
        result.push_back(unit);
    }
    return std::nullopt;
}

std::uint32_t inspect(const std::string &path) {
    const auto result = files().attributes(path);
    if (result.error) {
        return failure(result.error);
    }
    diagnostics::write_service_trace("TRACE native file attributes accepted\n");
    return result.value;
}

std::uint32_t AMBROSIA_GUEST_ABI attributes_wide(const void *source) noexcept {
    NativeErrorScope restore;
    if (!source) {
        return failure(87);
    }
    try {
        const auto path = read_filename<char16_t>(source);
        if (!path) {
            return failure(206);
        }
        const auto utf8 = unicode::utf16_to_utf8(*path, true);
        if (!utf8) {
            return failure(123);
        }
        return inspect(*utf8);
    } catch (const std::bad_alloc &) {
        return failure(8);
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

std::uint32_t AMBROSIA_GUEST_ABI attributes_bytes(const char *source) noexcept {
    NativeErrorScope restore;
    if (!source) {
        return failure(87);
    }
    try {
        const auto path = read_filename<char>(source);
        if (!path) {
            return failure(206);
        }
        return inspect(*path);
    } catch (const std::bad_alloc &) {
        return failure(8);
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

void *invalid_handle(std::uint32_t error) noexcept {
    set_last_error(error);
    return reinterpret_cast<void *>(~std::uintptr_t{0});
}

void *create_file(std::string_view path, std::uint32_t access, std::uint32_t share, const void *security,
                  std::uint32_t disposition, std::uint32_t flags, void *template_file) {
    auto *runtime = current_runtime();
    if (!runtime) {
        diagnostics::stop_trace("file creation requires a scoped native runtime", 78);
    }
    if (access != 0 && access != 0x80000000u) {
        return invalid_handle(5); // The mounted asset namespace never grants writes.
    }
    if (disposition != 3) {
        return invalid_handle(disposition >= 1 && disposition <= 5 ? 5 : 87);
    }
    if (share & ~7u) {
        return invalid_handle(87);
    }
    if (security || template_file || (flags & ~(0x20000000u | 0x80u | 1u))) {
        return invalid_handle(50);
    }
    const bool read_data = access != 0;
    auto opened = runtime->files().open(path, read_data, share, (flags & 0x20000000u) != 0);
    if (opened.error) {
        return invalid_handle(opened.error);
    }
    auto file = std::make_shared<ReadonlyFileObject>(std::move(opened.file), read_data);
    const auto result = runtime->objects().insert_waitable(file, access | object_synchronize);
    if (result.error) {
        return invalid_handle(result.error);
    }
    diagnostics::write_service_trace("TRACE native read-only file handle opened\n");
    return reinterpret_cast<void *>(result.handle);
}

void *AMBROSIA_GUEST_ABI create_file_wide(const void *path, std::uint32_t access, std::uint32_t share,
                                          const void *security, std::uint32_t disposition,
                                          std::uint32_t flags, void *template_file) noexcept {
    NativeErrorScope restore;
    if (!path) {
        return invalid_handle(87);
    }
    try {
        const auto wide = read_filename<char16_t>(path);
        if (!wide) {
            return invalid_handle(206);
        }
        const auto utf8 = unicode::utf16_to_utf8(*wide, true);
        return utf8 ? create_file(*utf8, access, share, security, disposition, flags, template_file)
                    : invalid_handle(123);
    } catch (const std::bad_alloc &) {
        return invalid_handle(8);
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

void *AMBROSIA_GUEST_ABI create_file_bytes(const char *path, std::uint32_t access, std::uint32_t share,
                                           const void *security, std::uint32_t disposition,
                                           std::uint32_t flags, void *template_file) noexcept {
    NativeErrorScope restore;
    if (!path) {
        return invalid_handle(87);
    }
    try {
        const auto bytes = read_filename<char>(path);
        return bytes ? create_file(*bytes, access, share, security, disposition, flags, template_file)
                     : invalid_handle(206);
    } catch (const std::bad_alloc &) {
        return invalid_handle(8);
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

std::shared_ptr<ReadonlyFileObject> file_object(void *handle) {
    auto *runtime = current_runtime();
    if (!runtime) {
        diagnostics::stop_trace("file metadata requires a scoped native runtime", 78);
    }
    const auto reference = runtime->objects().reference(reinterpret_cast<std::uintptr_t>(handle));
    return std::dynamic_pointer_cast<ReadonlyFileObject>(reference.object);
}

std::int32_t boolean_failure(std::uint32_t error) noexcept {
    set_last_error(error);
    return 0;
}

std::int32_t AMBROSIA_GUEST_ABI get_file_time(void *handle, void *creation_output, void *access_output,
                                              void *write_output) noexcept {
    NativeErrorScope restore;
    try {
        const auto file = file_object(handle);
        if (!file) {
            return boolean_failure(6);
        }
        if (!file->read_data()) {
            return boolean_failure(5);
        }
        const auto times = file->times();
        if (times.error != host::FileLookupError::none) {
            return boolean_failure(times.error == host::FileLookupError::unsupported ? 50 : 31);
        }
        if (creation_output && !times.creation) {
            return boolean_failure(50); // ctime is not creation time.
        }
        const auto access = windows_filetime(times.access);
        const auto write = windows_filetime(times.write);
        const auto creation =
            times.creation ? windows_filetime(*times.creation) : std::optional<std::uint64_t>{};
        if ((access_output && !access) || (write_output && !write) || (creation_output && !creation)) {
            return boolean_failure(534);
        }
        if (creation_output) {
            std::memcpy(creation_output, &*creation, sizeof(*creation));
        }
        if (access_output) {
            std::memcpy(access_output, &*access, sizeof(*access));
        }
        if (write_output) {
            std::memcpy(write_output, &*write, sizeof(*write));
        }
        diagnostics::write_service_trace("TRACE native file times retrieved\n");
        return 1;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

std::int32_t AMBROSIA_GUEST_ABI get_file_size(void *handle, void *output) noexcept {
    NativeErrorScope restore;
    if (!output) {
        return boolean_failure(87);
    }
    try {
        const auto file = file_object(handle);
        if (!file) {
            return boolean_failure(6);
        }
        const auto found = file->inspect();
        if (found.error != host::FileLookupError::none) {
            return boolean_failure(31);
        }
        if (found.metadata.bytes > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return boolean_failure(534);
        }
        const auto bytes = static_cast<std::int64_t>(found.metadata.bytes);
        std::memcpy(output, &bytes, sizeof(bytes));
        diagnostics::write_service_trace("TRACE native file size retrieved\n");
        return 1;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}
} // namespace

WaitResult ReadonlyFileObject::wait(std::uint32_t milliseconds) {
    (void)milliseconds;
    // There are no pending I/O operations on these synchronous source handles.
    return {wait_object_0, 0};
}

host::FileLookup ReadonlyFileObject::inspect() const {
    return file_->inspect();
}

host::FileTimes ReadonlyFileObject::times() const {
    return file_->times();
}

std::optional<std::uint64_t> windows_filetime(host::FileTimestamp timestamp) noexcept {
    constexpr std::int64_t epoch_seconds = 11644473600;
    constexpr std::uint64_t ticks_per_second = 10000000;
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (timestamp.nanoseconds >= 1000000000 || timestamp.seconds < -epoch_seconds ||
        timestamp.seconds > static_cast<std::int64_t>(maximum / ticks_per_second) - epoch_seconds) {
        return std::nullopt;
    }
    const auto seconds = static_cast<std::uint64_t>(timestamp.seconds + epoch_seconds);
    const auto fraction = timestamp.nanoseconds / 100;
    if (seconds > (maximum - fraction) / ticks_per_second) {
        return std::nullopt;
    }
    return seconds * ticks_per_second + fraction;
}

void register_file_services(ImportRegistry &registry) {
    for (const auto *module : {"kernel32.dll", "kernelbase.dll", "api-ms-win-core-file-l1-1-0.dll",
                               "api-ms-win-core-file-l1-2-0.dll"}) {
        registry.add(module, "GetFileTime", reinterpret_cast<std::uintptr_t>(&get_file_time));
        registry.add(module, "GetFileSizeEx", reinterpret_cast<std::uintptr_t>(&get_file_size));
        registry.add(module, "CreateFileW", reinterpret_cast<std::uintptr_t>(&create_file_wide));
        registry.add(module, "CreateFileA", reinterpret_cast<std::uintptr_t>(&create_file_bytes));
        registry.add(module, "GetFileAttributesW", reinterpret_cast<std::uintptr_t>(&attributes_wide));
        registry.add(module, "GetFileAttributesA", reinterpret_cast<std::uintptr_t>(&attributes_bytes));
    }
}
} // namespace ambrosia::services
