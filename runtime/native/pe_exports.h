#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>

namespace ambrosia::pe {
struct ExportTarget {
    std::uint32_t rva = 0;
    std::string forwarder;
};

struct ExportTable {
    std::map<std::string, ExportTarget> names;
    std::map<std::uint32_t, ExportTarget> ordinals;
};

ExportTable parse_exports(std::span<const std::byte> image, std::uint32_t directory_rva,
                          std::uint32_t directory_size);
} // namespace ambrosia::pe
