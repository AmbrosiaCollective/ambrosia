#pragma once
#include <stdexcept>
#include <string>

namespace ambrosia::guest {
template <class Character> std::basic_string<Character> read_string(const Character *source) {
    std::basic_string<Character> value;
    for (std::size_t index = 0; index < 32768; ++index) {
        const auto character = source[index];
        if (!character) {
            return value;
        }
        value.push_back(character);
    }
    throw std::runtime_error("guest string exceeds the runtime string budget");
}
} // namespace ambrosia::guest
