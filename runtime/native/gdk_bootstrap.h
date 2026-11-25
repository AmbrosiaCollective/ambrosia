#pragma once
#include "import_registry.h"
#include <mutex>
#include <set>
#include <string>
#include <utility>

namespace ambrosia::services {
inline constexpr std::int32_t gdk_invalid_argument = static_cast<std::int32_t>(0x80070057u);
inline constexpr std::int32_t gdk_out_of_memory = static_cast<std::int32_t>(0x8007000eu);

struct GdkBootstrapSnapshot {
    std::size_t references = 0;
    std::size_t versions = 0;
    std::uint64_t error_reports = 0;
    std::int32_t last_error = 0;
};

// Offline process-owned initialization, not an online Gaming Services client.
// API classes/queues have separate implementations and capability boundaries.
class GdkBootstrap {
  public:
    std::int32_t initialize(std::uint32_t gdk_version, std::uint32_t service_version);
    void uninitialize();
    std::int32_t report_error(std::int32_t code, std::string message);
    GdkBootstrapSnapshot snapshot() const;
    std::string last_error_message() const;

  private:
    mutable std::mutex mutex_;
    std::set<std::pair<std::uint32_t, std::uint32_t>> versions_;
    std::size_t references_ = 0;
    std::uint64_t reports_ = 0;
    std::int32_t last_error_ = 0;
    std::string last_message_;
};

void register_gdk_bootstrap(ImportRegistry &registry);
} // namespace ambrosia::services
