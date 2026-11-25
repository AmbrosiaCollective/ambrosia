#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>

namespace ambrosia::services {
struct GraphicsMemoryPlan {
    std::uint32_t range_class = 0;
    std::uint64_t covered_bytes = 0;
};

// This records the observed console configuration before backend creation.
// It neither reserves memory nor establishes GPU capability. A future backend
// must consume the plan and validate/reserve its real address range separately.
class GraphicsMemoryPolicy {
  public:
    // Keep configuration and backend creation serialized. Failure/destruction
    // releases the gate without marking a device as successfully created.
    class DeviceCreation {
      public:
        DeviceCreation(const DeviceCreation &) = delete;
        DeviceCreation &operator=(const DeviceCreation &) = delete;
        std::optional<GraphicsMemoryPlan> plan() const;
        void succeeded();

      private:
        friend class GraphicsMemoryPolicy;
        explicit DeviceCreation(GraphicsMemoryPolicy &owner);
        GraphicsMemoryPolicy &owner_;
        std::unique_lock<std::mutex> lock_;
    };

    std::int32_t configure(std::span<const std::byte> description);
    std::optional<GraphicsMemoryPlan> snapshot() const;
    DeviceCreation begin_device_creation();

  private:
    mutable std::mutex mutex_;
    std::optional<GraphicsMemoryPlan> plan_;
    bool device_created_ = false;
};
} // namespace ambrosia::services
