#include "scan_services.h"
#include "guest_strings.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include <algorithm>
#include <cerrno>
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

struct TextField {
    bool suppressed = false;
    std::size_t width = 0;
    char conversion = 0;
};

TextField text_field(std::string_view format, std::size_t &position) {
    TextField field;
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
    // In a narrow scanner, h forces narrow c/s independently of the legacy
    // wide-specifier option. Wide/multibyte conversions need a separate adapter.
    if (position < format.size() && format[position] == 'h') {
        ++position;
    }
    if (position == format.size() || (format[position] != 's' && format[position] != 'c')) {
        throw std::runtime_error("unsupported CRT scanning conversion");
    }
    field.conversion = format[position++];
    return field;
}

std::int32_t scan_text(std::string_view input, std::string_view format, Destinations &destinations) {
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
        const auto field = text_field(format, position);
        char *destination = nullptr;
        if (!field.suppressed) {
            destination = destinations.next();
            if (!destination) {
                throw InvalidScanParameter{};
            }
        }
        if (field.conversion == 's') {
            while (consumed < input.size() && space(input[consumed])) {
                ++consumed;
            }
        }
        if (consumed == input.size()) {
            return input_failure();
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
        return scan_text(bounded_input(input, count), guest::read_string(format), destinations);
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
