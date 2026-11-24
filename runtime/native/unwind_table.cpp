// Validation adapted from inherited pe_loader.cpp::register_unwind at f732bb5;
// project GPL-3.0-or-later. Lookup uses standard C++ algorithms. See REUSE.md.
#include "unwind_table.h"
#include "pe_bytes.h"
#include <algorithm>
#include <stdexcept>

namespace ambrosia::pe {
UnwindTable UnwindTable::parse(std::span<const std::byte> bytes, std::uint32_t image_bytes) {
    constexpr std::size_t record_bytes = 12;
    constexpr std::size_t entry_limit = 1024 * 1024;
    if (!image_bytes || bytes.size() % record_bytes || bytes.size() / record_bytes > entry_limit) {
        throw std::runtime_error("invalid x64 unwind table size or entry budget");
    }
    UnwindTable result;
    result.entries_.reserve(bytes.size() / record_bytes);
    std::uint32_t previous_end = 0;
    for (std::size_t offset = 0; offset < bytes.size(); offset += record_bytes) {
        const RuntimeFunction function{detail::read<std::uint32_t>(bytes, offset),
                                       detail::read<std::uint32_t>(bytes, offset + 4),
                                       detail::read<std::uint32_t>(bytes, offset + 8)};
        if (function.end < function.begin || function.end > image_bytes || function.begin < previous_end) {
            throw std::runtime_error("x64 unwind functions overlap, are unordered or exceed the image");
        }
        // The indirect form points to another RUNTIME_FUNCTION, not UNWIND_INFO.
        // It needs a checked image reader and cycle detection before support.
        if (function.unwind & 1) {
            throw std::runtime_error("indirect x64 runtime-function entries are unsupported");
        }
        if (!function.unwind || function.unwind % 4 || function.unwind > image_bytes ||
            image_bytes - function.unwind < 4) {
            throw std::runtime_error("x64 unwind header is unaligned or outside the image");
        }
        result.entries_.push_back(function);
        previous_end = function.end;
    }
    return result;
}

const RuntimeFunction *UnwindTable::find(std::uint32_t pc_rva) const {
    auto found = std::upper_bound(entries_.begin(), entries_.end(), pc_rva,
                                  [](auto pc, const auto &function) { return pc < function.begin; });
    if (found == entries_.begin()) {
        return nullptr;
    }
    --found;
    // EndAddress is exclusive; zero-length records never cover an address.
    return pc_rva < found->end ? &*found : nullptr;
}
} // namespace ambrosia::pe
