#pragma once
#include "package_identity.h"
#include "package_installation.h"
#include "readonly_files.h"
#include "runtime_interface.h"
#include "task_queues.h"
#include <array>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>

namespace ambrosia::services {
inline constexpr Guid package_class_id{
    0xaf406016, 0xe850, 0x4aa8, {0xa8, 0x8d, 0x2f, 0x3d, 0xcb, 0x9d, 0xac, 0x7e}};
inline constexpr Guid package_interface_id{
    0x0b9d556e, 0xab70, 0x43f4, {0xba, 0xa0, 0x99, 0x99, 0xac, 0x4c, 0xb0, 0x40}};
inline constexpr std::int32_t no_current_package = static_cast<std::int32_t>(0x80073d54u);

struct InstallationProgress {
    std::uint64_t total_bytes = 0;
    std::uint64_t installed_bytes = 0;
    std::uint64_t launch_bytes = 0;
    bool launchable = false;
    bool completed = false;
    std::array<std::byte, 6> reserved{};
    bool operator==(const InstallationProgress &) const = default;
};

static_assert(sizeof(InstallationProgress) == 32 && offsetof(InstallationProgress, launchable) == 24 &&
              offsetof(InstallationProgress, completed) == 25);

// Process identity for a validated loose package. This does not assert that
// installation chunks, DLC, licensing or online services are available.
class GdkPackages {
  public:
    static constexpr unsigned monitor_limit = 128;
    static constexpr unsigned registration_limit = 128;
    explicit GdkPackages(std::shared_ptr<HandleIds> ids = std::make_shared<HandleIds>());
    ~GdkPackages();
    void bind(const ReadonlyFiles &files);
    void bind_installation(std::span<const std::byte> inventory, const ReadonlyFiles &files);
    std::shared_ptr<const PackageInstallation> installation() const;
    std::int32_t query(const Guid &interface_id, void *output) noexcept;
    std::int32_t identifier(std::size_t bytes, char *output) const;
    bool is_packaged() const;
    std::int32_t create_monitor(std::string_view identifier, std::uint32_t selectors, std::uint32_t interval,
                                NativeQueueScheduler scheduler, void *output,
                                NativeQueueScheduler completion = {});
    void close_monitor(std::uintptr_t handle);
    InstallationProgress progress(std::uintptr_t handle) const;
    bool update_monitor(std::uintptr_t handle);
    std::int32_t register_progress(std::uintptr_t monitor, std::function<void()> delivery, void *token);
    bool unregister_progress(std::uintptr_t monitor, std::uintptr_t token, bool wait);
    void quiesce();

    bool owns_interface(const void *object) const noexcept {
        return object == &object_;
    }

    std::uint32_t references() const noexcept {
        return object_.references.load();
    }

  private:
    struct Monitor;
    struct Registration;
    struct RegistrationBudget;
    std::shared_ptr<Monitor> find_monitor(std::uintptr_t handle) const;
    RuntimeInterface object_;
    std::shared_ptr<HandleIds> ids_;
    std::shared_ptr<RegistrationBudget> registration_budget_;
    mutable std::mutex mutex_;
    bool configured_ = false;
    bool quiescing_ = false;
    std::optional<PackageIdentity> identity_;
    std::shared_ptr<const PackageInstallation> installation_;
    std::map<std::uintptr_t, std::shared_ptr<Monitor>> monitors_;
};
} // namespace ambrosia::services
