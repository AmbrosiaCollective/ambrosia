#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace ambrosia::services {
struct LocaleIdentifier {
    std::uint32_t value;
    bool neutral;
};

// Only accept known, lossless ICU/Windows LCID mappings. Names with no exact
// table entry remain unsupported instead of falling back to another region.
std::optional<LocaleIdentifier> lookup_locale_identifier(std::string_view language_tag);

// Explicit guest formatting preference, independent of host identity/geography
// and the CRT's initial C locale. Configuration ends at the first read.
class UserLocale {
  public:
    static constexpr std::size_t name_capacity = 85; // Win32 LOCALE_NAME_MAX_LENGTH, including NUL.
    void configure(std::string_view language_tag);
    std::u16string name() const;

  private:
    mutable std::mutex mutex_;
    std::u16string name_ = u"en-US";
    bool configured_ = false;
    mutable bool observed_ = false;
};
} // namespace ambrosia::services
