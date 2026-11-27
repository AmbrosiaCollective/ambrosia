// Shared mapping lifetime used by the loader and explicit execution session.
#pragma once
#include "pe_exports.h"
#include "pe_image.h"
#include "unwind_table.h"

namespace ambrosia::pe {
struct Image::State {
    explicit State(std::size_t size) : memory(size) {}

    host::Mapping memory;

    struct SectionRange {
        std::size_t rva = 0;
        std::size_t bytes = 0;
        host::Access access = host::Access::none;
    };

    std::vector<SectionRange> section_ranges;
    std::uint32_t entry = 0;
    bool dll = false;
    bool has_tls = false;
    std::atomic<bool> thread_notifications{true};
    bool notify_guest(std::uint32_t reason);
    bool inspection_only = false;
    std::uint32_t unsupported_directories = 0;
    std::size_t unresolved_imports = 0;
    UnwindTable unwind;
    guest::TlsTemplate tls;
    std::uint32_t tls_slot = 0;
    ExportTable exports;
    std::vector<std::uint32_t> callbacks;
    std::vector<Import> imports;
    std::atomic<bool> active{false};
};
} // namespace ambrosia::pe
