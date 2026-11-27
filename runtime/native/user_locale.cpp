#include "user_locale.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <unicode/uloc.h>
#include <utility>

namespace ambrosia::services {
namespace {
struct ParsedLocale {
    std::array<char, 512> identifier{};
    std::u16string name;
};

ParsedLocale parse_locale(std::string_view tag) {
    if (tag.empty() || tag.size() >= UserLocale::name_capacity ||
        !std::all_of(tag.begin(), tag.end(), [](unsigned char unit) {
            return (unit >= 'a' && unit <= 'z') || (unit >= 'A' && unit <= 'Z') ||
                   (unit >= '0' && unit <= '9') || unit == '-';
        })) {
        throw std::invalid_argument("locale-name requires a bounded BCP47 language tag");
    }
    const std::string input(tag);
    ParsedLocale result;
    std::int32_t parsed = 0;
    UErrorCode status = U_ZERO_ERROR;
    const auto locale_length =
        uloc_forLanguageTag(input.c_str(), result.identifier.data(),
                            static_cast<std::int32_t>(result.identifier.size()), &parsed, &status);
    // ICU can successfully parse only the valid prefix of a malformed tag.
    // The guest profile accepts the whole supplied name or rejects it.
    if (U_FAILURE(status) || parsed != static_cast<std::int32_t>(tag.size()) || locale_length < 0 ||
        locale_length >= static_cast<std::int32_t>(result.identifier.size())) {
        throw std::invalid_argument("locale-name is not a complete supported BCP47 language tag");
    }
    std::array<char, UserLocale::name_capacity> canonical{};
    status = U_ZERO_ERROR;
    const auto length = uloc_toLanguageTag(result.identifier.data(), canonical.data(),
                                           static_cast<std::int32_t>(canonical.size()), true, &status);
    if (U_FAILURE(status) || length <= 0 || length >= static_cast<std::int32_t>(canonical.size())) {
        throw std::invalid_argument("canonical locale-name exceeds the guest name contract");
    }
    result.name.assign(canonical.begin(), canonical.begin() + length);
    return result;
}
} // namespace

std::optional<LocaleIdentifier> lookup_locale_identifier(std::string_view language_tag) {
    const auto locale = parse_locale(language_tag);
    const auto identifier = uloc_getLCID(locale.identifier.data());
    if (!identifier) {
        return std::nullopt;
    }
    std::array<char, 512> restored{};
    UErrorCode status = U_ZERO_ERROR;
    const auto restored_length = uloc_getLocaleForLCID(identifier, restored.data(),
                                                       static_cast<std::int32_t>(restored.size()), &status);
    if (U_FAILURE(status) || restored_length < 0 ||
        restored_length >= static_cast<std::int32_t>(restored.size())) {
        return std::nullopt;
    }
    std::array<char, UserLocale::name_capacity> restored_tag{};
    status = U_ZERO_ERROR;
    const auto length = uloc_toLanguageTag(restored.data(), restored_tag.data(),
                                           static_cast<std::int32_t>(restored_tag.size()), true, &status);
    if (U_FAILURE(status) || length <= 0 || length >= static_cast<std::int32_t>(restored_tag.size()) ||
        std::u16string(restored_tag.begin(), restored_tag.begin() + length) != locale.name) {
        return std::nullopt;
    }
    std::array<char, ULOC_COUNTRY_CAPACITY> country{};
    status = U_ZERO_ERROR;
    const auto country_length = uloc_getCountry(locale.identifier.data(), country.data(),
                                                static_cast<std::int32_t>(country.size()), &status);
    if (U_FAILURE(status) || country_length < 0 ||
        country_length >= static_cast<std::int32_t>(country.size())) {
        return std::nullopt;
    }
    return LocaleIdentifier{identifier, country_length == 0};
}

void UserLocale::configure(std::string_view language_tag) {
    auto locale = parse_locale(language_tag);
    const std::lock_guard guard(mutex_);
    if (configured_ || observed_) {
        throw std::runtime_error("guest user locale must be selected once before its first read");
    }
    name_ = std::move(locale.name);
    configured_ = true;
}

std::u16string UserLocale::name() const {
    const std::lock_guard guard(mutex_);
    observed_ = true;
    return name_;
}
} // namespace ambrosia::services
