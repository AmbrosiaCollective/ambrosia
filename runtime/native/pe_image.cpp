#include "pe_image.h"
#include "image_state.h"
#include "pe_bytes.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <string_view>

namespace ambrosia::pe {
namespace {
using detail::read;
using detail::require_range;
using detail::string_at;

[[noreturn]] void fail(const char *reason) {
    throw std::runtime_error(reason);
}

void write64(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value) {
    require_range(offset, sizeof(value), bytes.size());
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

bool power_of_two(std::uint32_t value) {
    return value && !(value & (value - 1));
}

struct Directory {
    std::uint32_t rva = 0, size = 0;
};

struct Section {
    std::uint32_t rva, size, raw, raw_size, flags;
    std::size_t pages;
};

host::Access section_access(std::uint32_t characteristics) {
    if (characteristics & 0x20000000) {
        return host::Access::read_execute;
    }
    if (characteristics & 0x80000000) {
        return host::Access::read_write;
    }
    if (characteristics & 0x40000000) {
        return host::Access::read;
    }
    return host::Access::none;
}
} // namespace

Image Image::load(std::span<const std::byte> bytes, const Resolver &resolver, std::uint32_t tls_slot) {
    if (tls_slot >= 64) {
        fail("module TLS slot exceeds bootstrap limit");
    }
    if (bytes.size() > 64 * 1024 * 1024) {
        fail("bootstrap PE file exceeds 64 MiB limit");
    }
    if (read<std::uint16_t>(bytes, 0) != 0x5a4d) {
        fail("not a DOS/PE image");
    }
    const auto nt = read<std::uint32_t>(bytes, 0x3c);
    require_range(nt, 24, bytes.size());
    if (read<std::uint32_t>(bytes, nt) != 0x4550 || read<std::uint16_t>(bytes, nt + 4) != 0x8664) {
        fail("bootstrap requires an AMD64 PE image");
    }
    const auto count = read<std::uint16_t>(bytes, nt + 6);
    const auto optional_size = read<std::uint16_t>(bytes, nt + 20);
    const auto characteristics = read<std::uint16_t>(bytes, nt + 22);
    if (!count || count > 96 || !(characteristics & 2)) {
        fail("bootstrap requires an executable PE image");
    }
    const bool dll = (characteristics & 0x2000) != 0;
    const std::size_t optional = nt + 24;
    require_range(optional, optional_size, bytes.size());
    if (optional_size < 112 || read<std::uint16_t>(bytes, optional) != 0x20b) {
        fail("bootstrap requires PE32+ headers");
    }
    const auto entry = read<std::uint32_t>(bytes, optional + 16);
    const auto preferred = read<std::uint64_t>(bytes, optional + 24);
    const auto alignment = read<std::uint32_t>(bytes, optional + 32);
    const auto file_alignment = read<std::uint32_t>(bytes, optional + 36);
    const auto image_size = read<std::uint32_t>(bytes, optional + 56);
    const auto headers = read<std::uint32_t>(bytes, optional + 60);
    const auto directories = read<std::uint32_t>(bytes, optional + 108);
    const auto page = host::Mapping::page_size();
    if (!power_of_two(alignment) || alignment < page || alignment % page || !power_of_two(file_alignment) ||
        file_alignment < 512 || file_alignment > 65536 || file_alignment > alignment || !image_size ||
        image_size > 64 * 1024 * 1024 || image_size % alignment || !headers || headers % file_alignment ||
        headers > image_size) {
        fail("unsupported PE size/alignment");
    }
    require_range(0, headers, bytes.size());
    if (directories > 16 || 112 + directories * 8 > optional_size) {
        fail("PE directory table exceeds optional header");
    }
    const auto section_table = optional + optional_size;
    require_range(section_table, std::size_t(count) * 40, headers);
    std::array<Directory, 16> dirs{};
    for (std::uint32_t i = 0; i < directories; ++i) {
        dirs[i] = {read<std::uint32_t>(bytes, optional + 112 + i * 8),
                   read<std::uint32_t>(bytes, optional + 116 + i * 8)};
        if (!dirs[i].rva && !dirs[i].size) {
            continue;
        }
        if (!dirs[i].rva || !dirs[i].size) {
            fail("incomplete PE directory");
        }
        // Guest exception/unwind handling and delayed imports remain unsupported.
        if (i != 0 && i != 1 && i != 5 && i != 9 && i != 12) {
            fail("PE directory needs an unimplemented guest service");
        }
        require_range(dirs[i].rva, dirs[i].size, image_size);
    }

    const auto header_pages = (headers + page - 1) / page * page;
    std::vector<Section> sections;
    bool executable_entry = false;
    for (std::uint16_t i = 0; i < count; ++i) {
        const auto at = section_table + i * 40;
        Section section{read<std::uint32_t>(bytes, at + 12), read<std::uint32_t>(bytes, at + 8),
                        read<std::uint32_t>(bytes, at + 20), read<std::uint32_t>(bytes, at + 16),
                        read<std::uint32_t>(bytes, at + 36), 0};
        section.size = std::max(section.size, section.raw_size);
        if (!section.size || section.rva % alignment || section.rva < header_pages ||
            (section.raw_size &&
             (section.raw < headers || section.raw % file_alignment || section.raw_size % file_alignment))) {
            fail("invalid PE section placement");
        }
        require_range(section.rva, section.size, image_size);
        require_range(section.raw, section.raw_size, bytes.size());
        section.pages = (section.size + page - 1) / page * page;
        require_range(section.rva, section.pages, image_size);
        if ((section.flags & 0xa0000000u) == 0xa0000000u) {
            fail("writable executable section refused");
        }
        for (const auto &other : sections) {
            if (section.rva < other.rva + other.pages && other.rva < section.rva + section.pages) {
                fail("overlapping PE virtual sections");
            }
            if (section.raw_size && other.raw_size && section.raw < other.raw + other.raw_size &&
                other.raw < section.raw + section.raw_size) {
                fail("overlapping PE file sections");
            }
        }
        if (entry >= section.rva && entry - section.rva < section.size && (section.flags & 0x20000000)) {
            executable_entry = true;
        }
        sections.push_back(section);
    }
    if (!executable_entry && !(dll && entry == 0)) {
        fail("PE entry is not in an executable section");
    }
    Image image(image_size);
    auto &state = *image.state_;
    state.entry = entry;
    state.dll = dll;
    state.tls_slot = tls_slot;
    std::span<std::byte> mapped(state.memory.data(), image_size);
    std::memcpy(mapped.data(), bytes.data(), headers);
    for (const auto &section : sections) {
        if (section.raw_size) {
            std::memcpy(mapped.data() + section.rva, bytes.data() + section.raw, section.raw_size);
        }
    }

    const auto base = reinterpret_cast<std::uintptr_t>(mapped.data());
    const auto delta = std::uint64_t(base) - preferred;
    if (delta && ((characteristics & 1) || !dirs[5].size)) {
        fail("PE cannot relocate to host allocation");
    }
    if (dirs[5].size) {
        std::size_t cursor = dirs[5].rva, end = cursor + dirs[5].size;
        while (cursor < end) {
            require_range(cursor, 8, end);
            const auto block_page = read<std::uint32_t>(mapped, cursor);
            const auto block_size = read<std::uint32_t>(mapped, cursor + 4);
            if (block_page % 4096 || block_size < 8 || block_size % 2) {
                fail("invalid PE relocation block");
            }
            require_range(cursor, block_size, end);
            for (std::size_t at = cursor + 8; at < cursor + block_size; at += 2) {
                const auto item = read<std::uint16_t>(mapped, at);
                if (!(item >> 12)) {
                    continue;
                }
                if ((item >> 12) != 10) {
                    fail("unsupported PE relocation type");
                }
                const std::uint64_t target = std::uint64_t(block_page) + (item & 0xfff);
                require_range(target, 8, mapped.size());
                write64(mapped, target, read<std::uint64_t>(mapped, target) + delta);
            }
            cursor += block_size;
        }
    }

    if (dirs[1].size) {
        bool terminated = false;
        for (std::size_t descriptor = dirs[1].rva; descriptor + 20 <= std::size_t(dirs[1].rva) + dirs[1].size;
             descriptor += 20) {
            std::array<std::uint32_t, 5> values;
            for (unsigned i = 0; i < 5; ++i) {
                values[i] = read<std::uint32_t>(mapped, descriptor + i * 4);
            }
            if (std::all_of(values.begin(), values.end(), [](auto v) { return v == 0; })) {
                terminated = true;
                break;
            }
            Import import;
            import.module = string_at(mapped, values[3]);
            if (import.module.find_first_of("/\\:") != std::string::npos) {
                fail("import module must be a basename");
            }
            for (auto &ch : import.module) {
                if (ch >= 'A' && ch <= 'Z') {
                    ch += 'a' - 'A';
                }
            }
            const std::size_t lookup = values[0] ? values[0] : values[4];
            if (!lookup || !values[4]) {
                fail("missing PE import thunk table");
            }
            bool thunk_end = false;
            for (std::size_t index = 0; index < mapped.size() / 8; ++index) {
                const auto thunk = read<std::uint64_t>(mapped, lookup + index * 8);
                if (!thunk) {
                    thunk_end = true;
                    break;
                }
                import.by_ordinal = bool(thunk >> 63);
                import.name.clear();
                import.ordinal = 0;
                if (import.by_ordinal) {
                    if (thunk & 0x7fffffffffff0000ull) {
                        fail("invalid PE ordinal import");
                    }
                    import.ordinal = static_cast<std::uint16_t>(thunk);
                } else {
                    if (thunk > 0xfffffffdull) {
                        fail("invalid PE import name RVA");
                    }
                    require_range(thunk, 2, mapped.size());
                    import.name = string_at(mapped, static_cast<std::uint32_t>(thunk + 2));
                }
                const auto address = resolver ? resolver(import) : 0;
                if (!address) {
                    fail("unresolved PE import; execution refused");
                }
                write64(mapped, std::size_t(values[4]) + index * 8, address);
                state.imports.push_back(import);
            }
            if (!thunk_end) {
                fail("unterminated PE import thunk table");
            }
        }
        if (!terminated) {
            fail("unterminated PE import descriptors");
        }
    }

    if (dirs[9].size) {
        if (dirs[9].size < 40) {
            fail("truncated PE TLS directory");
        }
        const auto directory = dirs[9].rva;
        const auto start = read<std::uint64_t>(mapped, directory);
        const auto end = read<std::uint64_t>(mapped, directory + 8);
        const auto index = read<std::uint64_t>(mapped, directory + 16);
        const auto callbacks = read<std::uint64_t>(mapped, directory + 24);
        const auto zero_fill = read<std::uint32_t>(mapped, directory + 32);
        const auto flags = read<std::uint32_t>(mapped, directory + 36);
        if (flags & ~0x00f00000u) {
            fail("unsupported PE TLS flags");
        }
        const auto alignment_code = (flags >> 20) & 15;
        if (alignment_code == 15) {
            fail("invalid PE TLS alignment");
        }
        state.tls.alignment = alignment_code ? std::size_t(1) << (alignment_code - 1) : 16;

        auto rva_for = [&](std::uint64_t address, std::size_t size, std::uint32_t access) {
            if (address < base) {
                fail("TLS address is outside the mapped image");
            }
            const auto rva = address - base;
            require_range(rva, size, mapped.size());
            const bool valid = std::any_of(sections.begin(), sections.end(), [&](const auto &section) {
                return rva >= section.rva && rva - section.rva <= section.size &&
                       size <= section.size - (rva - section.rva) && (section.flags & access) == access;
            });
            if (!valid) {
                fail("TLS address has no compatible section");
            }
            return static_cast<std::uint32_t>(rva);
        };
        (void)rva_for(base + directory, 40, 0x40000000);
        if (start || end || index || callbacks || zero_fill) {
            state.has_tls = true;
            if (end < start || end - start > 1024 * 1024 || zero_fill > 1024 * 1024 - (end - start)) {
                fail("invalid PE TLS template size");
            }
            if (start || end) {
                const auto rva = rva_for(start, end - start, 0x40000000);
                state.tls.initial.assign(mapped.begin() + rva, mapped.begin() + rva + (end - start));
            }
            state.tls.zero_fill = zero_fill;
            const auto index_rva = rva_for(index, 4, 0x80000000);
            if (index_rva % 4) {
                fail("unaligned PE TLS index");
            }
            const std::uint32_t slot = tls_slot;
            std::memcpy(mapped.data() + index_rva, &slot, sizeof(slot));
            if (callbacks) {
                bool terminated = false;
                for (std::size_t i = 0; i <= 64; ++i) {
                    const auto callback_rva = rva_for(callbacks + i * 8, 8, 0x40000000);
                    const auto callback = read<std::uint64_t>(mapped, callback_rva);
                    if (!callback) {
                        terminated = true;
                        break;
                    }
                    if (i == 64) {
                        fail("too many PE TLS callbacks");
                    }
                    state.callbacks.push_back(rva_for(callback, 1, 0x20000000));
                }
                if (!terminated) {
                    fail("unterminated PE TLS callback table");
                }
            }
        }
    }

    state.exports = parse_exports(mapped, dirs[0].rva, dirs[0].size);
    for (const auto &[ordinal, target] : state.exports.ordinals) {
        (void)ordinal;
        if (!target.forwarder.empty()) {
            continue;
        }
        const bool mapped_target = std::any_of(sections.begin(), sections.end(), [&](const auto &section) {
            return target.rva >= section.rva && target.rva - section.rva < section.size &&
                   (section.flags & 0xe0000000u);
        });
        if (!mapped_target) {
            fail("PE export target is outside mapped sections");
        }
    }

    // No page is writable and executable at once. Parsing/binding completes
    // before code becomes executable; gaps remain inaccessible.
    state.memory.flush_instructions();
    state.memory.protect(0, image_size, host::Access::none);
    state.memory.protect(0, header_pages, host::Access::read);
    for (const auto &section : sections) {
        state.memory.protect(section.rva, section.pages, section_access(section.flags));
    }
    return image;
}

} // namespace ambrosia::pe
