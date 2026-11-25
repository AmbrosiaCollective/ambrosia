#pragma once
#include "import_registry.h"

namespace ambrosia::services {
// Nonnegative Unix epoch conversion, independent of native time_t or local calendars.
std::int64_t crt_seconds_from_filetime(std::uint64_t filetime) noexcept;
void register_clock_services(ImportRegistry &registry);
} // namespace ambrosia::services
