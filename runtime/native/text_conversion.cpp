#include "text_conversion.h"
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unicode/ustring.h>
#include <vector>

namespace ambrosia::unicode {
namespace {
std::int32_t input_length(std::size_t size) {
    if (size > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        throw std::length_error("Unicode input exceeds the conversion library's count range");
    }
    return static_cast<std::int32_t>(size);
}

bool malformed(UErrorCode error) {
    return error == U_INVALID_CHAR_FOUND || error == U_ILLEGAL_CHAR_FOUND || error == U_TRUNCATED_CHAR_FOUND;
}

void require_status(UErrorCode error, bool preflight) {
    if (preflight && error == U_BUFFER_OVERFLOW_ERROR) {
        return;
    }
    if (U_FAILURE(error)) {
        throw std::runtime_error("native Unicode conversion library failed");
    }
}
} // namespace

std::optional<std::u16string> utf8_to_utf16(std::string_view source, bool strict) {
    if (source.empty()) {
        return std::u16string{};
    }
    const auto length = input_length(source.size());
    const UChar32 replacement = strict ? -1 : 0xfffd;
    std::int32_t required = 0;
    UErrorCode error = U_ZERO_ERROR;
    u_strFromUTF8WithSub(nullptr, 0, &required, source.data(), length, replacement, nullptr, &error);
    if (malformed(error)) {
        return std::nullopt;
    }
    require_status(error, true);
    if (required < 0) {
        throw std::runtime_error("native Unicode conversion returned an invalid size");
    }
    std::vector<UChar> units(static_cast<std::size_t>(required));
    std::int32_t written = 0;
    error = U_ZERO_ERROR;
    u_strFromUTF8WithSub(units.data(), required, &written, source.data(), length, replacement, nullptr,
                         &error);
    require_status(error, false);
    if (written != required) {
        throw std::runtime_error("native Unicode size changed between conversion passes");
    }
    static_assert(sizeof(UChar) == sizeof(char16_t));
    std::u16string result(units.size(), u'\0');
    std::memcpy(result.data(), units.data(), units.size() * sizeof(UChar));
    return result;
}

std::optional<std::string> utf16_to_utf8(std::u16string_view source, bool strict) {
    if (source.empty()) {
        return std::string{};
    }
    const auto length = input_length(source.size());
    static_assert(sizeof(UChar) == sizeof(char16_t));
    // ICU's UChar typedef is configurable. Copy units rather than relying on
    // aliasing char16_t storage as an unrelated 16-bit native type.
    std::vector<UChar> units(source.size());
    std::memcpy(units.data(), source.data(), source.size() * sizeof(char16_t));
    const UChar32 replacement = strict ? -1 : 0xfffd;
    std::int32_t required = 0;
    UErrorCode error = U_ZERO_ERROR;
    u_strToUTF8WithSub(nullptr, 0, &required, units.data(), length, replacement, nullptr, &error);
    if (malformed(error)) {
        return std::nullopt;
    }
    require_status(error, true);
    if (required < 0) {
        throw std::runtime_error("native Unicode conversion returned an invalid size");
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    std::int32_t written = 0;
    error = U_ZERO_ERROR;
    u_strToUTF8WithSub(result.data(), required, &written, units.data(), length, replacement, nullptr, &error);
    require_status(error, false);
    if (written != required) {
        throw std::runtime_error("native Unicode size changed between conversion passes");
    }
    return result;
}
} // namespace ambrosia::unicode
