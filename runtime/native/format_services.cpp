#include "format_services.h"
#include "guest_strings.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include <algorithm>
#include <bit>
#include <cerrno>
#include <cfenv>
#include <cstring>
#include <stdexcept>
#include <string_view>
#ifndef FMT_HEADER_ONLY
#define FMT_HEADER_ONLY
#endif
#include <fmt/args.h>
#include <fmt/printf.h>

namespace ambrosia::services {
namespace {
constexpr std::uint64_t legacy_termination = 1;
constexpr std::uint64_t standard_snprintf = 2;
constexpr std::uint64_t legacy_wide_specifiers = 4;
constexpr std::uint64_t standard_rounding = 32;
constexpr std::uint64_t supported_options =
    legacy_termination | standard_snprintf | legacy_wide_specifiers | standard_rounding;
constexpr std::size_t field_limit = 32768;
constexpr std::size_t output_limit = 1024 * 1024;

class NativeErrorScope {
  public:
    NativeErrorScope() : saved_(errno) {}

    ~NativeErrorScope() {
        errno = saved_;
    }

  private:
    int saved_;
};

// A Windows x64 va_list points at consecutive eight-byte argument slots. It is
// not the host's register-save-area va_list. Default promotions already made
// float into double and narrow integer arguments into int at the guest call.
class Arguments {
  public:
    explicit Arguments(const std::byte *slots) : slots_(slots) {}

    std::uint64_t next() {
        if (!slots_ || used_ == 128) {
            throw std::runtime_error("CRT formatting argument budget or missing arguments");
        }
        std::uint64_t value;
        std::memcpy(&value, slots_ + used_ * sizeof(value), sizeof(value));
        ++used_;
        return value;
    }

  private:
    const std::byte *slots_;
    std::size_t used_ = 0;
};

bool contains(std::string_view values, char value) {
    return values.find(value) != std::string_view::npos;
}

std::size_t decimal(const std::string &format, std::size_t &position) {
    std::size_t result = 0;
    while (position < format.size() && format[position] >= '0' && format[position] <= '9') {
        result = result * 10 + static_cast<unsigned>(format[position++] - '0');
        if (result > field_limit) {
            throw std::runtime_error("CRT formatting field exceeds the runtime budget");
        }
    }
    return result;
}

std::string string_argument(std::uint64_t address, bool wide, std::int64_t precision) {
    if (!address) {
        return precision < 0 ? "(null)"
                             : std::string("(null)").substr(0, static_cast<std::size_t>(precision));
    }
    const auto limit = precision < 0 ? field_limit : static_cast<std::size_t>(precision);
    std::string result;
    for (std::size_t index = 0; index < limit; ++index) {
        const auto value = wide ? reinterpret_cast<const char16_t *>(address)[index]
                                : reinterpret_cast<const unsigned char *>(address)[index];
        if (!value) {
            return result;
        }
        if (wide && value > 127) {
            throw std::runtime_error("non-ASCII wide CRT formatting requires encoding-error support");
        }
        result.push_back(static_cast<char>(value));
    }
    if (precision < 0) {
        throw std::runtime_error("CRT formatting string exceeds the runtime budget");
    }
    return result;
}

std::string pad_string(std::string value, const std::string &flags, std::int64_t width) {
    if (width > static_cast<std::int64_t>(value.size())) {
        const auto padding = static_cast<std::size_t>(width) - value.size();
        if (contains(flags, '-')) {
            value.append(padding, ' ');
        } else {
            value.insert(0, padding, ' ');
        }
    }
    return value;
}

std::string render(const std::string &format, Arguments &arguments, std::uint64_t options) {
    fmt::dynamic_format_arg_store<fmt::printf_context> values;
    std::string translated;
    for (std::size_t position = 0; position < format.size();) {
        const auto character = format[position++];
        if (character != '%') {
            translated.push_back(character);
            continue;
        }
        if (position < format.size() && format[position] == '%') {
            translated += "%%";
            ++position;
            continue;
        }
        std::string flags;
        while (position < format.size() && contains("-+ #0", format[position])) {
            flags.push_back(format[position++]);
        }
        std::int64_t width = 0;
        if (position < format.size() && format[position] == '*') {
            ++position;
            width = static_cast<std::int32_t>(arguments.next());
            if (width < 0) {
                flags.push_back('-');
                width = -width;
            }
        } else {
            width = static_cast<std::int64_t>(decimal(format, position));
        }
        if (width > static_cast<std::int64_t>(field_limit)) {
            throw std::runtime_error("CRT formatting width exceeds the runtime budget");
        }
        std::int64_t precision = -1;
        if (position < format.size() && format[position] == '.') {
            ++position;
            if (position < format.size() && format[position] == '*') {
                ++position;
                precision = static_cast<std::int32_t>(arguments.next());
            } else {
                precision = static_cast<std::int64_t>(decimal(format, position));
            }
            if (precision > static_cast<std::int64_t>(field_limit)) {
                throw std::runtime_error("CRT formatting precision exceeds the runtime budget");
            }
        }
        std::string length;
        if (position < format.size() && contains("hljztLI", format[position])) {
            length.push_back(format[position++]);
            if ((length == "h" || length == "l") && position < format.size() &&
                format[position] == length[0]) {
                length.push_back(format[position++]);
            } else if (length == "I" && position + 1 < format.size() &&
                       (format.compare(position, 2, "32") == 0 || format.compare(position, 2, "64") == 0)) {
                length += format.substr(position, 2);
                position += 2;
            }
        }
        if (position == format.size()) {
            throw std::runtime_error("incomplete CRT formatting conversion");
        }
        auto conversion = format[position++];
        std::string host_length;
        if (contains("diuoxX", conversion) && length != "L") {
            const auto slot = arguments.next();
            const auto bits = length == "hh"                                       ? 8
                              : length == "h"                                      ? 16
                              : length.empty() || length == "l" || length == "I32" ? 32
                                                                                   : 64;
            const auto mask = bits == 64 ? ~std::uint64_t(0) : (std::uint64_t(1) << bits) - 1;
            if (precision == 0 && !(slot & mask)) {
                // fmt 9's printf path retains a zero digit at precision zero.
                // CRT suppresses it, except for alternate octal's required 0.
                std::string zero;
                if (conversion == 'o' && contains(flags, '#')) {
                    zero = "0";
                } else if (conversion == 'd' || conversion == 'i') {
                    if (contains(flags, '+')) {
                        zero = "+";
                    } else if (contains(flags, ' ')) {
                        zero = " ";
                    }
                }
                values.push_back(pad_string(std::move(zero), flags, width));
                flags.clear();
                width = 0;
                precision = -1;
                conversion = 's';
            } else {
                if (conversion == 'd' || conversion == 'i') {
                    const auto value = bits == 8    ? static_cast<std::int8_t>(slot)
                                       : bits == 16 ? static_cast<std::int16_t>(slot)
                                       : bits == 32 ? static_cast<std::int32_t>(slot)
                                                    : std::bit_cast<std::int64_t>(slot);
                    values.push_back(value);
                } else {
                    const auto value = bits == 8    ? static_cast<std::uint8_t>(slot)
                                       : bits == 16 ? static_cast<std::uint16_t>(slot)
                                       : bits == 32 ? static_cast<std::uint32_t>(slot)
                                                    : slot;
                    values.push_back(value);
                }
                host_length = "ll";
            }
        } else if (contains("aAeEfFgG", conversion) && (length.empty() || length == "l" || length == "L")) {
            if (!(options & standard_rounding) || std::fegetround() != FE_TONEAREST) {
                throw std::runtime_error(
                    "legacy or non-nearest CRT floating-point formatting is unsupported");
            }
            // Windows long double has the same representation as double.
            values.push_back(std::bit_cast<double>(arguments.next()));
        } else if ((conversion == 's' || conversion == 'S') &&
                   (length.empty() || length == "h" || length == "l")) {
            const auto wide = length == "l" || (conversion == 'S' && length != "h");
            values.push_back(pad_string(string_argument(arguments.next(), wide, precision), flags, width));
            // printf counts narrow bytes; fmt's general string width counts
            // Unicode display columns. Apply byte bounds/padding before fmt.
            flags.clear();
            width = 0;
            precision = -1;
            conversion = 's';
        } else if ((conversion == 'c' || conversion == 'C') &&
                   (length.empty() || length == "h" || length == "l")) {
            const auto wide = length == "l" || (conversion == 'C' && length != "h");
            const auto value = arguments.next();
            if (wide && (value & 0xffff) > 127) {
                throw std::runtime_error("non-ASCII wide CRT character formatting is unsupported");
            }
            values.push_back(pad_string(std::string(1, static_cast<char>(value)), flags, width));
            flags.clear();
            width = 0;
            precision = -1;
            conversion = 's';
        } else {
            // Includes positional parameters and disabled %n. Do not silently
            // reinterpret unsupported Windows-specific conversions as host ones.
            throw std::runtime_error("unsupported CRT formatting conversion");
        }
        translated += '%' + flags;
        if (width) {
            translated += std::to_string(width);
        }
        if (precision >= 0) {
            translated += '.' + std::to_string(precision);
        }
        translated += host_length;
        translated += conversion;
    }
    auto result = fmt::vsprintf(translated, values);
    if (result.size() > output_limit) {
        throw std::runtime_error("CRT formatting output exceeds the runtime budget");
    }
    return result;
}

std::int32_t AMBROSIA_GUEST_ABI common_vsprintf(std::uint64_t options, char *buffer, std::size_t size,
                                                const char *format, void *locale,
                                                const std::byte *slots) noexcept {
    NativeErrorScope native_error;
    try {
        if (!current_runtime()) {
            throw std::runtime_error("CRT formatting requires a scoped runtime");
        }
        if (!format || (!buffer && size)) {
            throw std::runtime_error("CRT formatting invalid-parameter dispatch is unsupported");
        }
        if (locale) {
            throw std::runtime_error("explicit CRT formatting locales are unsupported");
        }
        if ((options & ~supported_options) || (options & 3) == 3) {
            throw std::runtime_error("unsupported CRT formatting options=" + std::to_string(options));
        }
        Arguments arguments(slots);
        const auto output = render(guest::read_string(format), arguments, options);
        // Standard snprintf returns the required count and always terminates a
        // nonempty destination. Legacy/default truncation contracts stay explicit
        // until independently verified rather than returning a guessed result.
        const bool standard = options & standard_snprintf;
        if (!standard && output.size() >= size) {
            throw std::runtime_error("legacy CRT formatting truncation is not implemented");
        }
        if (size) {
            const auto written = std::min(output.size(), size - 1);
            std::memcpy(buffer, output.data(), written);
            buffer[written] = '\0';
        }
        return static_cast<std::int32_t>(output.size());
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}
} // namespace

void register_format_services(ImportRegistry &registry) {
    for (const auto *module : {"ucrtbase.dll", "api-ms-win-crt-stdio-l1-1-0.dll"}) {
        registry.add(module, "__stdio_common_vsprintf", reinterpret_cast<std::uintptr_t>(&common_vsprintf));
    }
}
} // namespace ambrosia::services
