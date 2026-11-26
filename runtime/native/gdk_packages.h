#pragma once
#include "package_identity.h"
#include "readonly_files.h"
#include "runtime_interface.h"
#include <mutex>
#include <optional>

namespace ambrosia::services {
inline constexpr Guid package_class_id{
    0xaf406016, 0xe850, 0x4aa8, {0xa8, 0x8d, 0x2f, 0x3d, 0xcb, 0x9d, 0xac, 0x7e}};
inline constexpr Guid package_interface_id{
    0x0b9d556e, 0xab70, 0x43f4, {0xba, 0xa0, 0x99, 0x99, 0xac, 0x4c, 0xb0, 0x40}};
inline constexpr std::int32_t no_current_package = static_cast<std::int32_t>(0x80073d54u);

// Process identity for a validated loose package. This does not assert that
// installation chunks, DLC, licensing or online services are available.
class GdkPackages {
  public:
    GdkPackages();
    void bind(const ReadonlyFiles &files);
    std::int32_t query(const Guid &interface_id, void *output) noexcept;
    std::int32_t identifier(std::size_t bytes, char *output) const;
    bool is_packaged() const;

    bool owns_interface(const void *object) const noexcept {
        return object == &object_;
    }

    std::uint32_t references() const noexcept {
        return object_.references.load();
    }

  private:
    RuntimeInterface object_;
    mutable std::mutex mutex_;
    bool configured_ = false;
    std::optional<PackageIdentity> identity_;
};
} // namespace ambrosia::services
