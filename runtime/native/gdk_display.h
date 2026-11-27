#pragma once
#include "runtime_interface.h"
#include <atomic>

namespace ambrosia::services {
inline constexpr Guid display_class_id{
    0x03f0fe74, 0xfdd9, 0x4e5c, {0xb6, 0x30, 0xf9, 0x33, 0x9c, 0x47, 0xac, 0xc5}};
inline constexpr Guid display_interface_id{
    0x35f07670, 0x706e, 0x4bfb, {0x94, 0x76, 0x09, 0x07, 0x98, 0xc5, 0xeb, 0xf3}};

enum class DisplayHdrResult : std::uint32_t { unknown = 0, enabled = 1, disabled = 2 };

// Software output policy, not a host monitor or presented-surface inventory.
// The native path currently offers SDR only and has no HDR enable operation.
class GdkDisplay {
  public:
    GdkDisplay();
    void select_sdr_output();
    DisplayHdrResult try_enable_hdr(const void *self, std::uint32_t preference) const;
    std::int32_t query(const Guid &interface_id, void *output) noexcept;

  private:
    RuntimeInterface object_;
    std::atomic<bool> sdr_selected_{false};
};
} // namespace ambrosia::services
