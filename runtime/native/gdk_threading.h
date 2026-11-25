#pragma once
#include "runtime_interface.h"

namespace ambrosia::services {
inline constexpr Guid threading_class_id{
    0x073b7dcb, 0x1fcf, 0x4030, {0x94, 0xbe, 0xe3, 0xc9, 0xeb, 0x62, 0x34, 0x28}};

class GdkThreading {
  public:
    GdkThreading();
    std::int32_t query(const Guid &interface_id, void *output) noexcept;

    std::uint32_t references() const noexcept {
        return object_.references.load();
    }

  private:
    RuntimeInterface object_;
};
} // namespace ambrosia::services
