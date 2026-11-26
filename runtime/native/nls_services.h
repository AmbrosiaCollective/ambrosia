#pragma once
#include "import_registry.h"

namespace ambrosia::services {
// Explicit process policy, independent of the host locale and guest CRT C locale.
inline constexpr std::uint32_t native_ansi_codepage = 65001;
inline constexpr std::int32_t text_input_limit = 16 * 1024 * 1024;
void register_nls_services(ImportRegistry &registry);
} // namespace ambrosia::services
