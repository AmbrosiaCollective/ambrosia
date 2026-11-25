#include "service_module_image.h"
#include "pe_bytes.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace ambrosia::services {
namespace {
constexpr std::size_t image_limit = 512 * 1024;
constexpr std::size_t stub_size = 16;

void require_text(const std::string &text) {
    if (text.empty() || text.size() > 512 ||
        std::any_of(text.begin(), text.end(),
                    [](unsigned char character) { return character < 32 || character > 126; })) {
        throw std::runtime_error("native module metadata string is invalid or exceeds its budget");
    }
}

template <class T> void write(std::span<std::byte> image, std::size_t offset, T value) {
    pe::detail::require_range(offset, sizeof(T), image.size());
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        image[offset + index] = std::byte((std::uint64_t(value) >> (index * 8)) & 255);
    }
}

std::uint32_t append_string(std::span<std::byte> image, std::size_t &cursor, const std::string &text) {
    pe::detail::require_range(cursor, text.size() + 1, image.size());
    const auto rva = static_cast<std::uint32_t>(cursor);
    std::memcpy(image.data() + cursor, text.c_str(), text.size() + 1);
    cursor += text.size() + 1;
    return rva;
}

void section(std::span<std::byte> image, std::size_t offset, const char *name, std::uint32_t rva,
             std::uint32_t bytes, std::uint32_t pages, std::uint32_t flags) {
    std::memcpy(image.data() + offset, name, std::strlen(name));
    write(image, offset + 8, bytes);
    write(image, offset + 12, rva);
    write(image, offset + 16, pages);
    write(image, offset + 20, rva);
    write(image, offset + 36, flags);
}
} // namespace

ServiceModuleImage::ServiceModuleImage(std::string module, std::vector<ServiceExport> exports) {
    require_text(module);
    if (module.find_first_of("/\\:") != std::string::npos || exports.empty() || exports.size() > 4096) {
        throw std::runtime_error("native module name or export count is invalid");
    }
    std::sort(exports.begin(), exports.end(),
              [](const auto &left, const auto &right) { return left.name < right.name; });
    std::size_t direct = 0;
    std::size_t data_bytes = 40 + exports.size() * 10 + module.size() + 1;
    for (std::size_t index = 0; index < exports.size(); ++index) {
        const auto &entry = exports[index];
        require_text(entry.name);
        if ((entry.target != 0) == !entry.forwarder.empty() ||
            (index && entry.name == exports[index - 1].name)) {
            throw std::runtime_error("native module export is ambiguous or duplicated");
        }
        direct += entry.target != 0;
        data_bytes += entry.name.size() + 1;
        if (!entry.forwarder.empty()) {
            require_text(entry.forwarder);
            data_bytes += entry.forwarder.size() + 1;
        }
    }
    const auto page = host::Mapping::page_size();
    if (page < 4096 || page > 65536 || (page & (page - 1))) {
        throw std::runtime_error("native module image requires a supported host page alignment");
    }
    const auto rounded = [page](std::size_t size) { return (size + page - 1) & ~(page - 1); };
    const auto code_bytes = direct * stub_size;
    const auto code_pages = rounded(code_bytes);
    const auto data_rva = page + code_pages;
    const auto data_pages = rounded(data_bytes);
    if (data_rva + data_pages > image_limit) {
        throw std::runtime_error("native module image exceeds its byte budget");
    }
    memory_ = std::make_unique<host::Mapping>(data_rva + data_pages);
    std::span<std::byte> image(memory_->data(), memory_->size());
    constexpr std::size_t optional = 0x98;
    write<std::uint16_t>(image, 0, 0x5a4d);
    write<std::uint32_t>(image, 0x3c, 0x80);
    write<std::uint32_t>(image, 0x80, 0x4550);
    write<std::uint16_t>(image, 0x84, 0x8664);
    write<std::uint16_t>(image, 0x86, direct ? 2 : 1);
    write<std::uint16_t>(image, 0x94, 240);
    write<std::uint16_t>(image, 0x96, 0x2022);
    write<std::uint16_t>(image, optional, 0x20b);
    write<std::uint32_t>(image, optional + 4, code_pages);
    write<std::uint32_t>(image, optional + 8, data_pages);
    write<std::uint32_t>(image, optional + 20, direct ? page : 0);
    write<std::uint64_t>(image, optional + 24, base());
    write<std::uint32_t>(image, optional + 32, page);
    write<std::uint32_t>(image, optional + 36, 512);
    write<std::uint16_t>(image, optional + 40, 6);
    write<std::uint16_t>(image, optional + 48, 6);
    write<std::uint32_t>(image, optional + 56, image.size());
    write<std::uint32_t>(image, optional + 60, page);
    write<std::uint16_t>(image, optional + 68, 3);
    write<std::uint16_t>(image, optional + 70, 0x100);
    write<std::uint64_t>(image, optional + 72, 1024 * 1024);
    write<std::uint64_t>(image, optional + 80, page);
    write<std::uint64_t>(image, optional + 88, 1024 * 1024);
    write<std::uint64_t>(image, optional + 96, page);
    write<std::uint32_t>(image, optional + 108, 16);
    write<std::uint32_t>(image, optional + 112, data_rva);
    write<std::uint32_t>(image, optional + 116, data_bytes);
    auto section_offset = std::size_t{0x188};
    if (direct) {
        section(image, section_offset, ".text", page, code_bytes, code_pages, 0x60000020);
        section_offset += 40;
    }
    section(image, section_offset, ".edata", data_rva, data_bytes, data_pages, 0x40000040);

    const auto functions = data_rva + 40;
    const auto names = functions + exports.size() * 4;
    const auto ordinals = names + exports.size() * 4;
    auto cursor = ordinals + exports.size() * 2;
    write<std::uint32_t>(image, data_rva + 12, append_string(image, cursor, module));
    write<std::uint32_t>(image, data_rva + 16, 1);
    write<std::uint32_t>(image, data_rva + 20, exports.size());
    write<std::uint32_t>(image, data_rva + 24, exports.size());
    write<std::uint32_t>(image, data_rva + 28, functions);
    write<std::uint32_t>(image, data_rva + 32, names);
    write<std::uint32_t>(image, data_rva + 36, ordinals);
    std::size_t stub = 0;
    for (std::size_t index = 0; index < exports.size(); ++index) {
        const auto &entry = exports[index];
        std::uint32_t target;
        if (entry.target) {
            target = static_cast<std::uint32_t>(page + stub++ * stub_size);
            // ENDBR64; MOV RAX, target; JMP RAX. Tail dispatch preserves the
            // caller's Microsoft x64 registers, shadow space and stack args.
            constexpr std::array<unsigned char, 16> code{0xf3, 0x0f, 0x1e, 0xfa, 0x48, 0xb8, 0,    0,
                                                         0,    0,    0,    0,    0,    0,    0xff, 0xe0};
            std::memcpy(image.data() + target, code.data(), code.size());
            write<std::uint64_t>(image, target + 6, entry.target);
        } else {
            target = append_string(image, cursor, entry.forwarder);
        }
        write(image, functions + index * 4, target);
        write<std::uint32_t>(image, names + index * 4, append_string(image, cursor, entry.name));
        write<std::uint16_t>(image, ordinals + index * 2, index);
    }
    if (cursor != data_rva + data_bytes) {
        throw std::runtime_error("native module export layout does not match its checked budget");
    }
    exports_ = pe::parse_exports(image, static_cast<std::uint32_t>(data_rva),
                                 static_cast<std::uint32_t>(data_bytes));
    memory_->flush_instructions();
    memory_->protect(0, memory_->size(), host::Access::read);
    if (code_pages) {
        memory_->protect(page, code_pages, host::Access::read_execute);
    }
}

std::uintptr_t ServiceModuleImage::direct_export(const std::string &name) const {
    const auto found = exports_.names.find(name);
    if (found == exports_.names.end() || !found->second.forwarder.empty()) {
        return 0;
    }
    return base() + found->second.rva;
}
} // namespace ambrosia::services
