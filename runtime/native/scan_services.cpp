#include "scan_services.h"
#include "guest_strings.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace ambrosia::services {
namespace {
constexpr std::uint64_t legacy_wide_specifiers = 2;
constexpr std::size_t input_limit = 32768;
constexpr std::size_t conversion_limit = 128;

bool space(char character) {
    return character == ' ' || character == '\t' || character == '\n' || character == '\r' ||
           character == '\f' || character == '\v';
}

std::string bounded_input(const char *input, std::size_t count) {
    std::string result;
    for (std::size_t index = 0; index < std::min(count, input_limit); ++index) {
        if (!input[index]) {
            return result;
        }
        result.push_back(input[index]);
    }
    if (count > input_limit) {
        throw std::runtime_error("CRT scanning input exceeds the runtime budget");
    }
    return result;
}

class Destinations {
  public:
    explicit Destinations(const std::byte *slots) : slots_(slots) {}

    char *next() {
        if (!slots_ || used_ == conversion_limit) {
            throw std::runtime_error("CRT scanning argument budget or missing arguments");
        }
        // Windows x64 va_list is an array of eight-byte slots, even when the
        // destination's element is a byte. Never pass it to a host va_list API.
        std::uintptr_t address;
        static_assert(sizeof(address) == 8);
        std::memcpy(&address, slots_ + used_ * sizeof(address), sizeof(address));
        ++used_;
        return reinterpret_cast<char *>(address);
    }

  private:
    const std::byte *slots_;
    std::size_t used_ = 0;
};

struct InvalidScanParameter {};

struct ScanField {
    bool suppressed = false;
    std::size_t width = 0;
    std::string length;
    char conversion = 0;
};

ScanField scan_field(std::string_view format, std::size_t &position) {
    ScanField field;
    if (position < format.size() && format[position] == '*') {
        field.suppressed = true;
        ++position;
    }
    const auto width_start = position;
    while (position < format.size() && format[position] >= '0' && format[position] <= '9') {
        field.width = field.width * 10 + static_cast<unsigned>(format[position++] - '0');
        if (field.width > input_limit) {
            throw std::runtime_error("CRT scanning width exceeds the runtime budget");
        }
    }
    if (position != width_start && !field.width) {
        throw InvalidScanParameter{};
    }
    if (position < format.size() &&
        std::string_view("hljztI").find(format[position]) != std::string_view::npos) {
        field.length.push_back(format[position++]);
        if ((field.length == "h" || field.length == "l") && position < format.size() &&
            format[position] == field.length.front()) {
            field.length.push_back(format[position++]);
        } else if (field.length == "I" && position + 1 < format.size() &&
                   (format.substr(position, 2) == "32" || format.substr(position, 2) == "64")) {
            field.length.append(format.substr(position, 2));
            position += 2;
        }
    }
    if (position == format.size() ||
        std::string_view("scdiuoxX").find(format[position]) == std::string_view::npos) {
        throw std::runtime_error("unsupported CRT scanning conversion");
    }
    field.conversion = format[position++];
    if ((field.conversion == 's' || field.conversion == 'c') && !field.length.empty() &&
        field.length != "h") {
        throw std::runtime_error("unsupported CRT scanning text length modifier");
    }
    return field;
}

unsigned integer_bits(const ScanField &field) {
    if (field.length == "hh") {
        return 8;
    }
    if (field.length == "h") {
        return 16;
    }
    if (field.length.empty() || field.length == "l" || field.length == "I32") {
        return 32;
    }
    // LLP64: Windows long stays 32-bit even on a 64-bit Linux host. The
    // remaining supported modifiers describe 64-bit integers or pointer sizes.
    return 64;
}

bool scan_integer(std::string_view input, std::size_t &consumed, const ScanField &field, char *destination) {
    const auto width = field.width ? field.width : input_limit;
    const auto end = std::min(input.size(), consumed + width);
    auto position = consumed;
    bool negative = false;
    if (position < end && (input[position] == '+' || input[position] == '-')) {
        negative = input[position] == '-';
        ++position;
    }
    unsigned base = 10;
    if (field.conversion == 'o') {
        base = 8;
    } else if (field.conversion == 'x' || field.conversion == 'X') {
        base = 16;
    } else if (field.conversion == 'i' && position < end && input[position] == '0') {
        base = 8;
        if (position + 1 < end && (input[position + 1] == 'x' || input[position + 1] == 'X')) {
            base = 16;
        }
    }
    if (base == 16 && position + 1 < end && input[position] == '0' &&
        (input[position + 1] == 'x' || input[position + 1] == 'X')) {
        position += 2;
    }
    std::uint64_t magnitude = 0;
    const auto parsed = std::from_chars(input.data() + position, input.data() + end, magnitude, base);
    if (parsed.ec == std::errc::invalid_argument) {
        return false;
    }
    if (parsed.ec != std::errc{}) {
        throw std::runtime_error("CRT scanning integer exceeds the supported value range");
    }
    const auto bits = integer_bits(field);
    const auto signed_conversion = field.conversion == 'd' || field.conversion == 'i';
    auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (signed_conversion) {
        maximum = (std::uint64_t(1) << (bits - 1)) - (negative ? 0 : 1);
    } else if (bits < 64) {
        maximum = (std::uint64_t(1) << bits) - 1;
    }
    if (magnitude > maximum) {
        throw std::runtime_error("CRT scanning integer exceeds the supported destination range");
    }
    const auto value = negative ? std::uint64_t(0) - magnitude : magnitude;
    if (!field.suppressed) {
        // Destination pointers need not have native alignment. Copy exactly the
        // Windows type width; never let a host long overwrite adjacent fields.
        if (bits == 8) {
            const auto narrowed = static_cast<std::uint8_t>(value);
            std::memcpy(destination, &narrowed, sizeof(narrowed));
        } else if (bits == 16) {
            const auto narrowed = static_cast<std::uint16_t>(value);
            std::memcpy(destination, &narrowed, sizeof(narrowed));
        } else if (bits == 32) {
            const auto narrowed = static_cast<std::uint32_t>(value);
            std::memcpy(destination, &narrowed, sizeof(narrowed));
        } else {
            std::memcpy(destination, &value, sizeof(value));
        }
    }
    consumed = static_cast<std::size_t>(parsed.ptr - input.data());
    return true;
}

std::int32_t scan_buffer(std::string_view input, std::string_view format, Destinations &destinations) {
    std::size_t consumed = 0;
    std::size_t conversions = 0;
    std::int32_t assigned = 0;
    bool converted = false;
    const auto input_failure = [&] { return converted ? assigned : -1; };
    for (std::size_t position = 0; position < format.size();) {
        auto character = format[position++];
        if (space(character)) {
            while (position < format.size() && space(format[position])) {
                ++position;
            }
            while (consumed < input.size() && space(input[consumed])) {
                ++consumed;
            }
            continue;
        }
        if (character != '%' || (position < format.size() && format[position] == '%')) {
            if (character == '%') {
                ++position;
            }
            if (consumed == input.size()) {
                return input_failure();
            }
            if (input[consumed] != character) {
                return assigned;
            }
            ++consumed;
            continue;
        }
        if (++conversions > conversion_limit) {
            throw std::runtime_error("CRT scanning conversion budget exceeded");
        }
        const auto field = scan_field(format, position);
        char *destination = nullptr;
        if (!field.suppressed) {
            destination = destinations.next();
            if (!destination) {
                throw InvalidScanParameter{};
            }
        }
        if (field.conversion != 'c') {
            while (consumed < input.size() && space(input[consumed])) {
                ++consumed;
            }
        }
        if (consumed == input.size()) {
            return input_failure();
        }
        if (field.conversion != 's' && field.conversion != 'c') {
            if (!scan_integer(input, consumed, field, destination)) {
                return assigned;
            }
            if (!field.suppressed) {
                ++assigned;
            }
            converted = true;
            continue;
        }
        const auto start = consumed;
        const auto width = field.width ? field.width : field.conversion == 'c' ? 1 : input_limit;
        while (consumed < input.size() && consumed - start < width &&
               (field.conversion == 'c' || !space(input[consumed]))) {
            ++consumed;
        }
        const auto size = consumed - start;
        if (field.conversion == 'c' && size != width) {
            return input_failure();
        }
        if (!field.suppressed) {
            // Like non-secure vsscanf, the caller owns sufficient storage.
            // Width bounds input bytes; %s also requires space for the NUL.
            std::memcpy(destination, input.data() + start, size);
            if (field.conversion == 's') {
                destination[size] = 0;
            }
            ++assigned;
        }
        converted = true;
    }
    return assigned;
}

std::int32_t AMBROSIA_GUEST_ABI common_vsscanf(std::uint64_t options, const char *input, std::size_t count,
                                               const char *format, void *locale,
                                               const std::byte *slots) noexcept {
    const auto saved_errno = errno;

    struct RestoreErrno {
        int value;

        ~RestoreErrno() {
            errno = value;
        }
    } restore{saved_errno};

    try {
        if (!current_runtime()) {
            throw std::runtime_error("CRT scanning requires a scoped runtime");
        }
        if (!input || !format) {
            throw InvalidScanParameter{};
        }
        if (locale || (options & ~legacy_wide_specifiers)) {
            throw std::runtime_error("unsupported CRT scanning locale/options");
        }
        Destinations destinations(slots);
        return scan_buffer(bounded_input(input, count), guest::read_string(format), destinations);
    } catch (const InvalidScanParameter &) {
        crt_error() = 22;
        dispatch_invalid_parameter();
        return -1;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}
} // namespace

void register_scan_services(ImportRegistry &registry) {
    for (const auto *module : {"ucrtbase.dll", "api-ms-win-crt-stdio-l1-1-0.dll"}) {
        registry.add(module, "__stdio_common_vsscanf", reinterpret_cast<std::uintptr_t>(&common_vsscanf));
    }
}
} // namespace ambrosia::services
