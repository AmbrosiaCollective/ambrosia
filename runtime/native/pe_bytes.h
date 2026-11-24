#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>

namespace ambrosia::pe::detail {
inline void require_range(std::size_t offset, std::size_t count, std::size_t size) {
    if (offset > size || count > size - offset) {
        throw std::runtime_error("PE range exceeds its container");
    }
}

template <class T> T read(std::span<const std::byte> bytes, std::size_t offset) {
    require_range(offset, sizeof(T), bytes.size());
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        value |= std::uint64_t(std::to_integer<unsigned char>(bytes[offset + index])) << (index * 8);
    }
    return static_cast<T>(value);
}

inline std::string string_at(std::span<const std::byte> bytes, std::uint32_t offset,
                             std::size_t end = std::string::npos) {
    end = std::min(end, bytes.size());
    require_range(offset, 1, end);
    const auto *begin = reinterpret_cast<const char *>(bytes.data() + offset);
    const auto count = std::min<std::size_t>(4096, end - offset);
    const auto *terminator = static_cast<const char *>(std::memchr(begin, 0, count));
    if (!terminator || terminator == begin) {
        throw std::runtime_error("PE string is empty or unterminated");
    }
    return {begin, terminator};
}
} // namespace ambrosia::pe::detail
