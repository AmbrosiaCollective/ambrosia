#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace ambrosia::unicode {
// Nullopt means malformed Unicode in strict mode. Resource/library failures
// throw; the ABI facade owns its processing budget and guest error mapping.
std::optional<std::u16string> utf8_to_utf16(std::string_view source, bool strict);
std::optional<std::string> utf16_to_utf8(std::u16string_view source, bool strict);
} // namespace ambrosia::unicode
