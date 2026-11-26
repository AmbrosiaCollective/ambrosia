#pragma once
#include "import_registry.h"
#include <array>
#include <mutex>

namespace ambrosia::services {
// Windows x64 WSADATA, including deterministic padding and no vendor pointer.
struct WinsockData {
    std::uint16_t version = 0;
    std::uint16_t highest_version = 0x0202;
    std::uint16_t maximum_sockets = 0;
    std::uint16_t maximum_datagram = 0;
    std::uint64_t vendor = 0;
    std::array<char, 257> description{};
    std::array<char, 129> status{};
    std::array<std::byte, 6> padding{};
};

static_assert(sizeof(WinsockData) == 408 && offsetof(WinsockData, description) == 16 &&
              offsetof(WinsockData, status) == 273);

// Compatibility-library initialization only. No host socket/transport starts.
// Winsock 1.x compatibility and socket I/O are separate unsupported capabilities.
class WinsockLifecycle {
  public:
    std::int32_t startup(std::uint16_t requested, void *output);
    bool cleanup();
    unsigned references() const;

  private:
    mutable std::mutex mutex_;
    unsigned references_ = 0;
};

void register_winsock_lifecycle(ImportRegistry &registry);
} // namespace ambrosia::services
