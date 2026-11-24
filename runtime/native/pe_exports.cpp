// Adapted from runtime/src/pe_loader.cpp::Loader::parse_exports at 17176bd.
// Inherited project code remains under the repository's GPL-3.0-or-later terms.
#include "pe_exports.h"
#include "pe_bytes.h"
#include <limits>

namespace ambrosia::pe {
ExportTable parse_exports(std::span<const std::byte> image, std::uint32_t directory_rva,
                          std::uint32_t directory_size) {
    using detail::read;
    using detail::require_range;
    ExportTable exports;
    if (!directory_size) {
        return exports;
    }
    require_range(directory_rva, directory_size, image.size());
    require_range(0, 40, directory_size);
    const std::size_t directory_end = std::size_t(directory_rva) + directory_size;
    const auto ordinal_base = read<std::uint32_t>(image, directory_rva + 16);
    const auto function_count = read<std::uint32_t>(image, directory_rva + 20);
    const auto name_count = read<std::uint32_t>(image, directory_rva + 24);
    const auto functions = read<std::uint32_t>(image, directory_rva + 28);
    const auto names = read<std::uint32_t>(image, directory_rva + 32);
    const auto ordinals = read<std::uint32_t>(image, directory_rva + 36);
    if (function_count > 65536 || name_count > 65536 ||
        (function_count && ordinal_base > std::numeric_limits<std::uint32_t>::max() - (function_count - 1))) {
        throw std::runtime_error("PE export table exceeds bootstrap limits");
    }
    require_range(functions, std::size_t(function_count) * 4, image.size());
    require_range(names, std::size_t(name_count) * 4, image.size());
    require_range(ordinals, std::size_t(name_count) * 2, image.size());

    auto target_for = [&](std::uint32_t index) {
        if (index >= function_count) {
            throw std::runtime_error("PE export name points past address table");
        }
        ExportTarget target;
        target.rva = read<std::uint32_t>(image, std::size_t(functions) + index * 4);
        if (target.rva >= directory_rva && target.rva < directory_end) {
            target.forwarder = detail::string_at(image, target.rva, directory_end);
        } else if (target.rva) {
            require_range(target.rva, 1, image.size());
        }
        return target;
    };
    for (std::uint32_t index = 0; index < function_count; ++index) {
        auto target = target_for(index);
        if (target.rva) {
            exports.ordinals.emplace(ordinal_base + index, std::move(target));
        }
    }
    for (std::uint32_t index = 0; index < name_count; ++index) {
        const auto name_rva = read<std::uint32_t>(image, std::size_t(names) + index * 4);
        auto name = detail::string_at(image, name_rva);
        auto target = target_for(read<std::uint16_t>(image, std::size_t(ordinals) + index * 2));
        if (target.rva && !exports.names.emplace(std::move(name), std::move(target)).second) {
            throw std::runtime_error("duplicate PE export name");
        }
    }
    return exports;
}
} // namespace ambrosia::pe
