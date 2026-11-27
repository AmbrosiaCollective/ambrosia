#include "string_services.h"
#include "invalid_parameters.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include <cerrno>
#include <charconv>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace ambrosia::services {
namespace {
std::uint32_t lowercase_ascii(std::uint32_t value) noexcept {
    return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
}

template <class Character> std::size_t AMBROSIA_GUEST_ABI string_length(const Character *text) noexcept {
    return std::char_traits<Character>::length(text);
}

template <class Character, bool IgnoreCase>
std::int32_t compare_units(const Character *left, const Character *right, std::size_t count) noexcept {
    if (count == 0) {
        return 0;
    }
    if constexpr (IgnoreCase) {
        if (!left || !right) {
            crt_error() = 22;
            diagnostics::stop_trace("CRT invalid-parameter dispatch is not implemented", 78);
        }
    }
    using UnsignedCharacter = std::make_unsigned_t<Character>;
    for (std::size_t index = 0; index < count; ++index) {
        auto first = static_cast<std::uint32_t>(static_cast<UnsignedCharacter>(left[index]));
        auto second = static_cast<std::uint32_t>(static_cast<UnsignedCharacter>(right[index]));
        if constexpr (IgnoreCase) {
            first = lowercase_ascii(first);
            second = lowercase_ascii(second);
        }
        if (first != second) {
            return static_cast<std::int32_t>(first) - static_cast<std::int32_t>(second);
        }
        if (first == 0) {
            return 0;
        }
    }
    return 0;
}

template <class Character, bool IgnoreCase = false>
std::int32_t AMBROSIA_GUEST_ABI string_compare(const Character *left, const Character *right) noexcept {
    return compare_units<Character, IgnoreCase>(left, right, std::numeric_limits<std::size_t>::max());
}

template <class Character, bool IgnoreCase = false>
std::int32_t AMBROSIA_GUEST_ABI string_compare_counted(const Character *left, const Character *right,
                                                       std::size_t count) noexcept {
    return compare_units<Character, IgnoreCase>(left, right, count);
}

template <class Character>
Character *AMBROSIA_GUEST_ABI string_copy(Character *destination, const Character *source) noexcept {
    std::size_t index = 0;
    for (;;) {
        const auto value = source[index];
        destination[index] = value;
        if (value == 0) {
            return destination;
        }
        ++index;
    }
}

template <class Character>
Character *AMBROSIA_GUEST_ABI string_copy_counted(Character *destination, const Character *source,
                                                  std::size_t count) noexcept {
    std::size_t index = 0;
    while (index < count && source[index] != 0) {
        destination[index] = source[index];
        ++index;
    }
    while (index < count) {
        destination[index] = Character{};
        ++index;
    }
    return destination;
}

char *AMBROSIA_GUEST_ABI string_append(char *destination, const char *source) noexcept {
    string_copy(destination + std::strlen(destination), source);
    return destination;
}

char *AMBROSIA_GUEST_ABI string_append_counted(char *destination, const char *source,
                                               std::size_t count) noexcept {
    auto *tail = destination + std::strlen(destination);
    std::size_t index = 0;
    while (index < count && source[index] != 0) {
        tail[index] = source[index];
        ++index;
    }
    tail[index] = '\0';
    return destination;
}

std::size_t AMBROSIA_GUEST_ABI string_reject_span(const char *text, const char *reject) noexcept {
    return std::strcspn(text, reject);
}

char *AMBROSIA_GUEST_ABI string_find_set(const char *text, const char *accept) noexcept {
    // The C ABI returns a pointer into the caller's string, not a new copy.
    return const_cast<char *>(std::strpbrk(text, accept));
}

std::int32_t AMBROSIA_GUEST_ABI wide_collate(const char16_t *left, const char16_t *right) noexcept {
    if (!left || !right) {
        crt_error() = 22;
        diagnostics::stop_trace("CRT invalid-parameter dispatch is not implemented", 78);
    }
    return compare_units<char16_t, false>(left, right, std::numeric_limits<std::size_t>::max());
}

char *AMBROSIA_GUEST_ABI secure_token(char *text, const char *delimiters, char **context) noexcept {
    const auto saved_error = errno;
    char *cursor = text;
    if (context && !cursor) {
        // Pointer storage can originate in a packed guest structure.
        std::memcpy(&cursor, context, sizeof(cursor));
    }
    if (!delimiters || !context || !cursor) {
        crt_error() = 22;
        dispatch_invalid_parameter();
        errno = saved_error;
        return nullptr;
    }
    cursor += std::strspn(cursor, delimiters);
    char *token = nullptr;
    if (*cursor) {
        token = cursor;
        cursor += std::strcspn(cursor, delimiters);
        if (*cursor) {
            *cursor = '\0';
            ++cursor;
        }
    }
    std::memcpy(context, &cursor, sizeof(cursor));
    errno = saved_error;
    return token;
}

std::int32_t AMBROSIA_GUEST_ABI decimal_integer(const char *text) noexcept {
    const auto saved_error = errno;
    try {
        if (!current_runtime()) {
            throw std::runtime_error("CRT decimal conversion requires a scoped runtime");
        }
        if (!text) {
            dispatch_invalid_parameter();
            crt_error() = 22;
            errno = saved_error;
            return 0;
        }
        std::size_t position = 0;
        const auto read = [&] {
            if (position == 32768) {
                throw std::runtime_error("CRT decimal input exceeds the runtime string budget");
            }
            return text[position];
        };
        auto character = read();
        // Only the guest C locale is supported: space, tab, LF, VT, FF and CR.
        while (character == ' ' || (character >= '\t' && character <= '\r')) {
            ++position;
            character = read();
        }
        const bool negative = character == '-';
        if (negative || character == '+') {
            ++position;
            character = read();
        }
        const auto first_digit = position;
        while (character >= '0' && character <= '9') {
            ++position;
            character = read();
        }
        std::uint32_t magnitude = 0;
        const auto conversion = std::from_chars(text + first_digit, text + position, magnitude, 10);
        std::int32_t result = 0;
        if (first_digit != position) {
            const auto maximum = std::uint32_t{INT32_MAX} + static_cast<std::uint32_t>(negative);
            if (conversion.ec == std::errc::result_out_of_range || magnitude > maximum) {
                crt_error() = 34;
                result = negative ? INT32_MIN : INT32_MAX;
            } else {
                const auto signed_value = static_cast<std::int64_t>(magnitude);
                result = static_cast<std::int32_t>(negative ? -signed_value : signed_value);
            }
        }
        diagnostics::write_service_trace("TRACE native CRT decimal converted\n");
        errno = saved_error;
        return result;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

void require_byte_or_eof(std::int32_t value) noexcept {
    if (value < -1 || value > 255) {
        diagnostics::stop_trace("C-locale classification requires an unsigned byte or EOF", 78);
    }
}

template <std::uint16_t Mask> std::int32_t AMBROSIA_GUEST_ABI classify_byte(std::int32_t value) noexcept {
    require_byte_or_eof(value);
    auto *runtime = current_runtime();
    if (!runtime) {
        diagnostics::stop_trace("CRT classification requires a scoped runtime", 78);
    }
    try {
        return runtime->locale().classification()[value] & Mask;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

std::int32_t AMBROSIA_GUEST_ABI lowercase_byte(std::int32_t value) noexcept {
    require_byte_or_eof(value);
    return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
}

std::int32_t AMBROSIA_GUEST_ABI uppercase_byte(std::int32_t value) noexcept {
    require_byte_or_eof(value);
    return value >= 'a' && value <= 'z' ? value - ('a' - 'A') : value;
}
} // namespace

void register_string_services(ImportRegistry &registry) {
    // C is the only available guest locale. Non-C collation and Unicode case
    // tables must be introduced together with any future locale selection.
    for (const auto *module : {"ucrtbase.dll", "msvcrt.dll", "api-ms-win-crt-string-l1-1-0.dll"}) {
        registry.add(module, "strlen", reinterpret_cast<std::uintptr_t>(&string_length<char>));
        registry.add(module, "wcslen", reinterpret_cast<std::uintptr_t>(&string_length<char16_t>));
        registry.add(module, "strcmp", reinterpret_cast<std::uintptr_t>(&string_compare<char>));
        registry.add(module, "strncmp", reinterpret_cast<std::uintptr_t>(&string_compare_counted<char>));
        registry.add(module, "wcscmp", reinterpret_cast<std::uintptr_t>(&string_compare<char16_t>));
        registry.add(module, "wcscoll", reinterpret_cast<std::uintptr_t>(&wide_collate));
        registry.add(module, "_stricmp", reinterpret_cast<std::uintptr_t>(&string_compare<char, true>));
        registry.add(module, "_strnicmp",
                     reinterpret_cast<std::uintptr_t>(&string_compare_counted<char, true>));
        registry.add(module, "_wcsicoll", reinterpret_cast<std::uintptr_t>(&string_compare<char16_t, true>));
        registry.add(module, "_wcsnicmp",
                     reinterpret_cast<std::uintptr_t>(&string_compare_counted<char16_t, true>));
        registry.add(module, "strcpy", reinterpret_cast<std::uintptr_t>(&string_copy<char>));
        registry.add(module, "strncpy", reinterpret_cast<std::uintptr_t>(&string_copy_counted<char>));
        registry.add(module, "wcsncpy", reinterpret_cast<std::uintptr_t>(&string_copy_counted<char16_t>));
        registry.add(module, "strcat", reinterpret_cast<std::uintptr_t>(&string_append));
        registry.add(module, "strncat", reinterpret_cast<std::uintptr_t>(&string_append_counted));
        registry.add(module, "strcspn", reinterpret_cast<std::uintptr_t>(&string_reject_span));
        registry.add(module, "strpbrk", reinterpret_cast<std::uintptr_t>(&string_find_set));
        registry.add(module, "strtok_s", reinterpret_cast<std::uintptr_t>(&secure_token));
        registry.add(module, "isalnum", reinterpret_cast<std::uintptr_t>(&classify_byte<0x107>));
        registry.add(module, "isalpha", reinterpret_cast<std::uintptr_t>(&classify_byte<0x103>));
        registry.add(module, "isdigit", reinterpret_cast<std::uintptr_t>(&classify_byte<0x04>));
        registry.add(module, "islower", reinterpret_cast<std::uintptr_t>(&classify_byte<0x02>));
        registry.add(module, "isspace", reinterpret_cast<std::uintptr_t>(&classify_byte<0x08>));
        registry.add(module, "isxdigit", reinterpret_cast<std::uintptr_t>(&classify_byte<0x80>));
        registry.add(module, "tolower", reinterpret_cast<std::uintptr_t>(&lowercase_byte));
        registry.add(module, "toupper", reinterpret_cast<std::uintptr_t>(&uppercase_byte));
    }
    for (const auto *module : {"ucrtbase.dll", "msvcrt.dll", "api-ms-win-crt-convert-l1-1-0.dll"}) {
        registry.add(module, "atoi", reinterpret_cast<std::uintptr_t>(&decimal_integer));
    }
}
} // namespace ambrosia::services
