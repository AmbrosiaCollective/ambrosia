#include "gdk_packages.h"
#include "gdk_bootstrap.h"
#include "guest_thread.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include <array>
#include <cerrno>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace ambrosia::services {
struct GdkPackages::Monitor : std::enable_shared_from_this<Monitor> {
    std::mutex mutex;
    std::shared_ptr<const PackageInstallation> source;
    NativeQueueScheduler scheduler;
    std::uint32_t interval;
    InstallationProgress snapshot;
    std::exception_ptr failure;
    bool closed = false;

    Monitor(std::shared_ptr<const PackageInstallation> installation, NativeQueueScheduler queue,
            std::uint32_t cadence)
        : source(std::move(installation)), scheduler(std::move(queue)), interval(cadence) {
        (void)refresh();
    }

    bool refresh() {
        const std::lock_guard lock(mutex);
        if (closed) {
            throw std::runtime_error("installation monitor is closed");
        }
        if (failure) {
            std::rethrow_exception(failure);
        }
        InstallationReadiness observed;
        try {
            observed = source->inspect();
        } catch (...) {
            failure = std::current_exception();
            throw;
        }
        InstallationProgress next;
        next.total_bytes = observed.total_bytes;
        next.installed_bytes = observed.installed_bytes;
        // No subset/launch-marker model is asserted: require the whole inventory.
        next.launch_bytes = observed.total_bytes;
        next.launchable = next.completed = observed.completed();
        const bool changed = next != snapshot;
        snapshot = next;
        return changed;
    }

    bool schedule() {
        const auto weak = weak_from_this();
        return scheduler(interval, weak, [weak] {
            if (const auto monitor = weak.lock()) {
                monitor->tick();
            }
        });
    }

    void tick() noexcept {
        try {
            (void)refresh();
            const std::lock_guard lock(mutex);
            if (!closed && !schedule()) {
                throw std::runtime_error("installation monitor cannot schedule its next refresh");
            }
        } catch (...) {
            const std::lock_guard lock(mutex);
            if (!closed) {
                failure = std::current_exception();
            }
        }
    }

    InstallationProgress read() {
        const std::lock_guard lock(mutex);
        if (closed) {
            throw std::runtime_error("installation monitor is closed");
        }
        if (failure) {
            std::rethrow_exception(failure);
        }
        return snapshot;
    }

    void close() {
        const std::lock_guard lock(mutex);
        closed = true;
    }
};

namespace {
constexpr auto not_supported = static_cast<std::int32_t>(0x80070032u);

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

void require_ordinary_thread() {
    const auto *thread = guest::ThreadContext::current();
    if (thread && thread->time_sensitive()) {
        diagnostics::stop_trace("installation monitor mutation is unsafe on a time-sensitive thread", 78);
    }
}

std::int32_t AMBROSIA_GUEST_ABI create_monitor(void *self, const char *identifier,
                                               std::uint32_t selector_count, const void *,
                                               std::uint32_t interval, std::uintptr_t queue,
                                               void *output) noexcept {
    NativeErrorScope restore;
    try {
        auto &owner = packages(self);
        require_ordinary_thread();
        if (!output) {
            return gdk_invalid_argument;
        }
        const std::uintptr_t empty = 0;
        std::memcpy(output, &empty, sizeof(empty));
        if (!identifier) {
            return gdk_invalid_argument;
        }
        std::size_t length = 0;
        while (length <= package_identifier_limit && identifier[length]) {
            ++length;
        }
        if (!length || length > package_identifier_limit) {
            return gdk_invalid_argument;
        }
        NativeQueueScheduler scheduler;
        if (queue) {
            if (!current_runtime()->task_queues().inspect(queue)) {
                return static_cast<std::int32_t>(0x80070006u);
            }
            scheduler = current_runtime()->task_queues().thread_pool_scheduler(queue);
        }
        return owner.create_monitor(std::string_view(identifier, length), selector_count, interval,
                                    std::move(scheduler), output);
    } catch (const std::bad_alloc &) {
        return gdk_out_of_memory;
    } catch (const std::system_error &) {
        return gdk_out_of_memory;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

void AMBROSIA_GUEST_ABI close_monitor(void *self, std::uintptr_t handle) noexcept {
    NativeErrorScope restore;
    try {
        packages(self).close_monitor(handle);
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

void AMBROSIA_GUEST_ABI installation_progress(void *self, std::uintptr_t handle, void *output) noexcept {
    NativeErrorScope restore;
    try {
        if (!output) {
            diagnostics::stop_trace("installation progress requires an output buffer", 78);
        }
        const auto progress = packages(self).progress(handle);
        std::memcpy(output, &progress, sizeof(progress));
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

bool AMBROSIA_GUEST_ABI update_monitor(void *self, std::uintptr_t handle) noexcept {
    NativeErrorScope restore;
    try {
        require_ordinary_thread();
        return packages(self).update_monitor(handle);
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
    result[5] = reinterpret_cast<std::uintptr_t>(&create_monitor);
    result[6] = reinterpret_cast<std::uintptr_t>(&close_monitor);
    result[7] = reinterpret_cast<std::uintptr_t>(&installation_progress);
    result[8] = reinterpret_cast<std::uintptr_t>(&update_monitor);
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

GdkPackages::GdkPackages(std::shared_ptr<HandleIds> ids)
    : object_(package_vtable.data(), package_class_id, package_interfaces), ids_(std::move(ids)) {
    if (!ids_) {
        throw std::invalid_argument("installation monitors require an owned handle directory");
    }
}

GdkPackages::~GdkPackages() {
    for (const auto &[handle, monitor] : monitors_) {
        (void)handle;
        monitor->close();
    }
}

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

std::int32_t GdkPackages::create_monitor(std::string_view identifier, std::uint32_t selectors,
                                         std::uint32_t interval, NativeQueueScheduler scheduler,
                                         void *output) {
    if (!output) {
        return gdk_invalid_argument;
    }
    const std::uintptr_t empty = 0;
    std::memcpy(output, &empty, sizeof(empty));
    const std::lock_guard lock(mutex_);
    if (!identity_ || identifier != identity_->name) {
        return gdk_invalid_argument;
    }
    if (selectors || !installation_ || (interval && !scheduler)) {
        return not_supported;
    }
    if (monitors_.size() >= monitor_limit) {
        return gdk_out_of_memory;
    }
    auto monitor = std::make_shared<Monitor>(installation_, std::move(scheduler), interval);
    const auto handle = ids_->allocate();
    if (!handle) {
        return gdk_out_of_memory;
    }
    monitors_.emplace(handle, monitor);
    try {
        if (interval && !monitor->schedule()) {
            monitors_.erase(handle);
            return gdk_out_of_memory;
        }
    } catch (...) {
        monitors_.erase(handle);
        throw;
    }
    std::memcpy(output, &handle, sizeof(handle));
    diagnostics::write_service_trace("TRACE native installation monitor created\n");
    return 0;
}

std::shared_ptr<GdkPackages::Monitor> GdkPackages::find_monitor(std::uintptr_t handle) const {
    const std::lock_guard lock(mutex_);
    const auto found = monitors_.find(handle);
    if (found == monitors_.end()) {
        throw std::runtime_error("installation monitor handle is invalid or stale");
    }
    return found->second;
}

void GdkPackages::close_monitor(std::uintptr_t handle) {
    const std::lock_guard lock(mutex_);
    const auto found = monitors_.find(handle);
    if (found != monitors_.end()) {
        found->second->close();
        monitors_.erase(found);
    }
}

InstallationProgress GdkPackages::progress(std::uintptr_t handle) const {
    return find_monitor(handle)->read();
}

bool GdkPackages::update_monitor(std::uintptr_t handle) {
    return find_monitor(handle)->refresh();
}
} // namespace ambrosia::services
