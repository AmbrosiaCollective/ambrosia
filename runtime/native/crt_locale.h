#pragma once
#include <array>
#include <cstdint>

namespace ambrosia::services {
class CrtLocale {
  public:
    // Guest categories are 0..5 (LC_ALL through LC_TIME), independently of host
    // libc category numbers. The guest default and only available locale is C.
    char *select(std::int32_t category, const char *requested) noexcept {
        if (category < 0 || category >= static_cast<std::int32_t>(names_.size())) {
            return nullptr;
        }
        if (requested && requested[0] && !(requested[0] == 'C' && requested[1] == '\0')) {
            return nullptr;
        }
        return names_[static_cast<unsigned>(category)].data();
    }

    std::uint32_t codepage() const noexcept {
        return 0;
    }

    std::int32_t max_multibyte_length() const noexcept {
        return 1;
    }

    char16_t **locale_names() noexcept {
        return wide_names_.data();
    }

    const std::uint16_t *classification() const noexcept {
        // Index -1 is EOF, followed by unsigned byte values 0..255.
        return classification_.data() + 1;
    }

  private:
    static constexpr std::array<std::uint16_t, 257> c_classification() {
        enum Flag : std::uint16_t {
            upper = 0x01,
            lower = 0x02,
            digit = 0x04,
            space = 0x08,
            punctuation = 0x10,
            control = 0x20,
            blank = 0x40,
            hexadecimal = 0x80,
            alphabetic = 0x100
        };

        std::array<std::uint16_t, 257> table{};
        for (unsigned character = 0; character < 128; ++character) {
            auto &flags = table[character + 1];
            if (character < 32 || character == 127) {
                flags |= control;
            }
            if (character == ' ' || (character >= 9 && character <= 13)) {
                flags |= space;
            }
            if (character == ' ') {
                flags |= blank;
            }
            if (character >= 'A' && character <= 'Z') {
                flags |= upper | alphabetic;
            }
            if (character >= 'a' && character <= 'z') {
                flags |= lower | alphabetic;
            }
            if (character >= '0' && character <= '9') {
                flags |= digit | hexadecimal;
            }
            if ((character >= 'A' && character <= 'F') || (character >= 'a' && character <= 'f')) {
                flags |= hexadecimal;
            }
            if (character >= '!' && character <= '~' && !(flags & (alphabetic | digit))) {
                flags |= punctuation;
            }
        }
        return table;
    }

    std::array<std::array<char, 2>, 6> names_{
        {{'C', '\0'}, {'C', '\0'}, {'C', '\0'}, {'C', '\0'}, {'C', '\0'}, {'C', '\0'}}};
    // The C locale uses CRT codepage zero, single-byte characters and no NLS
    // locale-name objects. Non-C selection is rejected before these invariants
    // could become false; no host locale names or codepages are borrowed.
    std::array<char16_t *, 6> wide_names_{};
    const std::array<std::uint16_t, 257> classification_ = c_classification();
};
} // namespace ambrosia::services
