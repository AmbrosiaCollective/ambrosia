#include "pe_image.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <string_view>

namespace ambrosia::pe {
namespace {
[[noreturn]] void fail(const char* reason) { throw std::runtime_error(reason); }
void range(std::size_t offset, std::size_t count, std::size_t size) {
    if (offset > size || count > size - offset) fail("PE range exceeds its container");
}
template<class T> T read(std::span<const std::byte> bytes, std::size_t offset) {
    range(offset, sizeof(T), bytes.size());
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i)
        value |= std::uint64_t(std::to_integer<unsigned char>(bytes[offset + i])) << (i * 8);
    return static_cast<T>(value);
}
void write64(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value) {
    range(offset, sizeof(value), bytes.size());
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}
std::string string_at(std::span<const std::byte> bytes, std::uint32_t offset) {
    range(offset, 1, bytes.size());
    const auto* begin = reinterpret_cast<const char*>(bytes.data() + offset);
    const auto count = std::min<std::size_t>(4096, bytes.size() - offset);
    const auto* end = static_cast<const char*>(std::memchr(begin, 0, count));
    if (!end || end == begin) fail("PE string is empty or unterminated");
    return {begin, end};
}
bool power_of_two(std::uint32_t value) { return value && !(value & (value - 1)); }
struct Directory { std::uint32_t rva = 0, size = 0; };
struct Section { std::uint32_t rva, size, raw, raw_size, flags; std::size_t pages; };
}

Image Image::load(std::span<const std::byte> bytes, const Resolver& resolver) {
    if (bytes.size() > 64 * 1024 * 1024) fail("bootstrap PE file exceeds 64 MiB limit");
    if (read<std::uint16_t>(bytes, 0) != 0x5a4d) fail("not a DOS/PE image");
    const auto nt = read<std::uint32_t>(bytes, 0x3c);
    range(nt, 24, bytes.size());
    if (read<std::uint32_t>(bytes, nt) != 0x4550 || read<std::uint16_t>(bytes, nt + 4) != 0x8664)
        fail("bootstrap requires an AMD64 PE image");
    const auto count = read<std::uint16_t>(bytes, nt + 6);
    const auto optional_size = read<std::uint16_t>(bytes, nt + 20);
    const auto characteristics = read<std::uint16_t>(bytes, nt + 22);
    if (!count || count > 96 || !(characteristics & 2) || (characteristics & 0x2000))
        fail("bootstrap requires an executable, not a DLL");
    const std::size_t optional = nt + 24;
    range(optional, optional_size, bytes.size());
    if (optional_size < 112 || read<std::uint16_t>(bytes, optional) != 0x20b)
        fail("bootstrap requires PE32+ headers");
    const auto entry = read<std::uint32_t>(bytes, optional + 16);
    const auto preferred = read<std::uint64_t>(bytes, optional + 24);
    const auto alignment = read<std::uint32_t>(bytes, optional + 32);
    const auto file_alignment = read<std::uint32_t>(bytes, optional + 36);
    const auto image_size = read<std::uint32_t>(bytes, optional + 56);
    const auto headers = read<std::uint32_t>(bytes, optional + 60);
    const auto directories = read<std::uint32_t>(bytes, optional + 108);
    const auto page = host::Mapping::page_size();
    if (!power_of_two(alignment) || alignment < page || alignment % page ||
        !power_of_two(file_alignment) || file_alignment < 512 || file_alignment > 65536 ||
        file_alignment > alignment || !image_size || image_size > 64 * 1024 * 1024 ||
        image_size % alignment || !headers || headers % file_alignment || headers > image_size)
        fail("unsupported PE size/alignment");
    range(0, headers, bytes.size());
    if (directories > 16 || 112 + directories * 8 > optional_size)
        fail("PE directory table exceeds optional header");
    const auto section_table = optional + optional_size;
    range(section_table, std::size_t(count) * 40, headers);
    std::array<Directory, 16> dirs{};
    for (std::uint32_t i = 0; i < directories; ++i) {
        dirs[i] = {read<std::uint32_t>(bytes, optional + 112 + i * 8),
                   read<std::uint32_t>(bytes, optional + 116 + i * 8)};
        if (!dirs[i].rva && !dirs[i].size) continue;
        if (!dirs[i].rva || !dirs[i].size) fail("incomplete PE directory");
        // Reject unsupported initialization/lifecycle features rather than
        // calling a retail entry point without TLS, SEH or delayed imports.
        if (i != 1 && i != 5 && i != 12) fail("PE directory needs an unimplemented guest service");
        range(dirs[i].rva, dirs[i].size, image_size);
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
            (section.raw_size && (section.raw < headers || section.raw % file_alignment ||
                                  section.raw_size % file_alignment)))
            fail("invalid PE section placement");
        range(section.rva, section.size, image_size);
        range(section.raw, section.raw_size, bytes.size());
        section.pages = (section.size + page - 1) / page * page;
        range(section.rva, section.pages, image_size);
        if ((section.flags & 0xa0000000u) == 0xa0000000u) fail("writable executable section refused");
        for (const auto& other : sections) {
            if (section.rva < other.rva + other.pages && other.rva < section.rva + section.pages)
                fail("overlapping PE virtual sections");
            if (section.raw_size && other.raw_size && section.raw < other.raw + other.raw_size &&
                other.raw < section.raw + section.raw_size) fail("overlapping PE file sections");
        }
        if (entry >= section.rva && entry - section.rva < section.size && (section.flags & 0x20000000))
            executable_entry = true;
        sections.push_back(section);
    }
    if (!executable_entry) fail("PE entry is not in an executable section");
    Image image(image_size);
    image.entry_ = entry;
    std::span<std::byte> mapped(image.memory_.data(), image_size);
    std::memcpy(mapped.data(), bytes.data(), headers);
    for (const auto& section : sections)
        if (section.raw_size)
            std::memcpy(mapped.data() + section.rva, bytes.data() + section.raw, section.raw_size);

    const auto base = reinterpret_cast<std::uintptr_t>(mapped.data());
    const auto delta = std::uint64_t(base) - preferred;
    if (delta && ((characteristics & 1) || !dirs[5].size)) fail("PE cannot relocate to host allocation");
    if (dirs[5].size) {
        std::size_t cursor = dirs[5].rva, end = cursor + dirs[5].size;
        while (cursor < end) {
            range(cursor, 8, end);
            const auto block_page = read<std::uint32_t>(mapped, cursor);
            const auto block_size = read<std::uint32_t>(mapped, cursor + 4);
            if (block_page % 4096 || block_size < 8 || block_size % 2) fail("invalid PE relocation block");
            range(cursor, block_size, end);
            for (std::size_t at = cursor + 8; at < cursor + block_size; at += 2) {
                const auto item = read<std::uint16_t>(mapped, at);
                if (!(item >> 12)) continue;
                if ((item >> 12) != 10) fail("unsupported PE relocation type");
                const std::uint64_t target = std::uint64_t(block_page) + (item & 0xfff);
                range(target, 8, mapped.size());
                write64(mapped, target, read<std::uint64_t>(mapped, target) + delta);
            }
            cursor += block_size;
        }
    }

    if (dirs[1].size) {
        bool terminated = false;
        for (std::size_t descriptor = dirs[1].rva;
             descriptor + 20 <= std::size_t(dirs[1].rva) + dirs[1].size; descriptor += 20) {
            std::array<std::uint32_t, 5> values;
            for (unsigned i = 0; i < 5; ++i) values[i] = read<std::uint32_t>(mapped, descriptor + i * 4);
            if (std::all_of(values.begin(), values.end(), [](auto v) { return v == 0; })) {
                terminated = true;
                break;
            }
            Import import;
            import.module = string_at(mapped, values[3]);
            if (import.module.find_first_of("/\\:") != std::string::npos) fail("import module must be a basename");
            for (auto& ch : import.module) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
            const std::size_t lookup = values[0] ? values[0] : values[4];
            if (!lookup || !values[4]) fail("missing PE import thunk table");
            bool thunk_end = false;
            for (std::size_t index = 0; index < mapped.size() / 8; ++index) {
                const auto thunk = read<std::uint64_t>(mapped, lookup + index * 8);
                if (!thunk) { thunk_end = true; break; }
                import.by_ordinal = bool(thunk >> 63);
                import.name.clear();
                import.ordinal = 0;
                if (import.by_ordinal) {
                    if (thunk & 0x7fffffffffff0000ull) fail("invalid PE ordinal import");
                    import.ordinal = static_cast<std::uint16_t>(thunk);
                } else {
                    if (thunk > 0xfffffffdull) fail("invalid PE import name RVA");
                    range(thunk, 2, mapped.size());
                    import.name = string_at(mapped, static_cast<std::uint32_t>(thunk + 2));
                }
                const auto address = resolver ? resolver(import) : 0;
                if (!address) fail("unresolved PE import; execution refused");
                write64(mapped, std::size_t(values[4]) + index * 8, address);
                image.imports_.push_back(import);
            }
            if (!thunk_end) fail("unterminated PE import thunk table");
        }
        if (!terminated) fail("unterminated PE import descriptors");
    }

    // No page is writable and executable at once. Parsing/binding completes
    // before code becomes executable; gaps remain inaccessible.
    image.memory_.flush_instructions();
    image.memory_.protect(0, image_size, host::Access::none);
    image.memory_.protect(0, header_pages, host::Access::read);
    for (const auto& section : sections) {
        const auto access = section.flags & 0x20000000 ? host::Access::read_execute :
            section.flags & 0x80000000 ? host::Access::read_write :
            section.flags & 0x40000000 ? host::Access::read : host::Access::none;
        image.memory_.protect(section.rva, section.pages, access);
    }
    return image;
}

std::uint64_t Image::run_bootstrap() const {
    if (!memory_.data() || !entry_) fail("bootstrap image is not mapped");
    using Entry = std::uint64_t (AMBROSIA_GUEST_ABI *)();
    const auto function = reinterpret_cast<Entry>(memory_.data() + entry_);
    return function();
}
} // namespace ambrosia::pe
