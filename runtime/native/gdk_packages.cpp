#include "gdk_packages.h"
#include "gdk_bootstrap.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ambrosia::services {
namespace {
struct NativeErrorScope {
    int saved = errno;

    ~NativeErrorScope() {
        errno = saved;
    }
};

GdkPackages &packages(void *self) {
    auto *runtime = current_runtime();
    if (!runtime || !runtime->gdk_packages().owns_interface(self)) {
        diagnostics::stop_trace("package method requires its scoped native runtime", 78);
    }
    return runtime->gdk_packages();
}

std::int32_t AMBROSIA_GUEST_ABI identifier(void *self, std::size_t bytes, char *output) noexcept {
    NativeErrorScope restore;
    try {
        return packages(self).identifier(bytes, output);
    } catch (const std::bad_alloc &) {
        return gdk_out_of_memory;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

bool AMBROSIA_GUEST_ABI is_packaged(void *self) noexcept {
    NativeErrorScope restore;
    try {
        return packages(self).is_packaged();
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

template <std::size_t Slot> [[noreturn]] void AMBROSIA_GUEST_ABI unsupported() noexcept {
    diagnostics::stop_trace("unsupported local package interface slot " + std::to_string(Slot), 78);
}

template <std::size_t... Slots> auto make_vtable(std::index_sequence<Slots...>) {
    std::array<std::uintptr_t, sizeof...(Slots)> result{
        reinterpret_cast<std::uintptr_t>(&unsupported<Slots>)...};
    result[0] = reinterpret_cast<std::uintptr_t>(&interface_query);
    result[1] = reinterpret_cast<std::uintptr_t>(&interface_add_reference);
    result[2] = reinterpret_cast<std::uintptr_t>(&interface_release);
    result[3] = reinterpret_cast<std::uintptr_t>(&identifier);
    result[4] = reinterpret_cast<std::uintptr_t>(&is_packaged);
    return result;
}

const auto package_vtable = make_vtable(std::make_index_sequence<42>{});
constexpr std::array package_interfaces{package_interface_id};

std::optional<PackageIdentity> read_identity(const ReadonlyFiles &files, const char *name,
                                             PackageDescriptor kind) {
    const auto opened = files.open(name);
    if (opened.error == 2) {
        return std::nullopt;
    }
    if (opened.error || !opened.file) {
        throw std::runtime_error("cannot open a declared package descriptor");
    }
    const auto found = opened.file->inspect();
    if (found.error != host::FileLookupError::none || !found.metadata.bytes ||
        found.metadata.bytes > package_descriptor_limit) {
        throw std::runtime_error("package descriptor metadata is invalid or exceeds its bound");
    }
    std::vector<std::byte> content(static_cast<std::size_t>(found.metadata.bytes));
    const auto read = opened.file->read(0, content);
    if (read.error != host::FileReadError::none || read.bytes != content.size()) {
        throw std::runtime_error("package descriptor changed or could not be read completely");
    }
    return parse_package_identity(content, kind);
}
} // namespace

GdkPackages::GdkPackages() : object_(package_vtable.data(), package_class_id, package_interfaces) {}

void GdkPackages::bind(const ReadonlyFiles &files) {
    const std::lock_guard lock(mutex_);
    if (configured_) {
        throw std::runtime_error("process package identity is already configured");
    }
    auto identity = read_identity(files, "MicrosoftGame.Config", PackageDescriptor::game_config);
    const auto manifest = read_identity(files, "AppxManifest.xml", PackageDescriptor::appx_manifest);
    if (identity && manifest && *identity != *manifest) {
        throw std::runtime_error("package descriptors disagree about process identity");
    }
    if (!identity) {
        identity = manifest;
    }
    identity_ = std::move(identity);
    configured_ = true;
}

void GdkPackages::bind_installation(std::span<const std::byte> inventory, const ReadonlyFiles &files) {
    const auto parsed = parse_package_inventory(inventory);
    const std::lock_guard lock(mutex_);
    if (!identity_ || parsed.identity != *identity_ || installation_) {
        throw std::runtime_error("package inventory must match an existing identity and bind only once");
    }
    auto installation = std::make_shared<PackageInstallation>(parsed, files);
    const auto readiness = installation->inspect();
    installation_ = std::move(installation);
    diagnostics::write_service_trace(
        "TRACE native loose package inventory files=" + std::to_string(readiness.expected_files) + " ready=" +
        std::to_string(readiness.ready_files) + " complete=" + (readiness.completed() ? "1\n" : "0\n"));
}

std::shared_ptr<const PackageInstallation> GdkPackages::installation() const {
    const std::lock_guard lock(mutex_);
    return installation_;
}

std::int32_t GdkPackages::query(const Guid &interface_id, void *output) noexcept {
    return interface_query(&object_, &interface_id, output);
}

std::int32_t GdkPackages::identifier(std::size_t bytes, char *output) const {
    if (!output) {
        return pointer_error;
    }
    const std::lock_guard lock(mutex_);
    if (!identity_) {
        return no_current_package;
    }
    if (bytes <= identity_->name.size()) {
        return static_cast<std::int32_t>(0x8007007au);
    }
    std::memcpy(output, identity_->name.c_str(), identity_->name.size() + 1);
    diagnostics::write_service_trace("TRACE native local package identifier retrieved\n");
    return 0;
}

bool GdkPackages::is_packaged() const {
    const std::lock_guard lock(mutex_);
    return identity_.has_value();
}
} // namespace ambrosia::services
