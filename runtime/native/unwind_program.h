// Checked metadata and stack restoration, independent of host signal machinery.
#pragma once
#include "unwind_table.h"
#include <array>
#include <optional>

namespace ambrosia::pe {
struct UnwindOperation {
    std::uint8_t code_offset = 0;
    std::uint8_t opcode = 0;
    std::uint8_t info = 0;
    std::uint32_t operand = 0;
};

struct RegisterContext {
    // AMD64 encoding order: RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8..R15.
    std::array<std::uint64_t, 16> integer{};
    std::array<std::array<std::byte, 16>, 16> xmm{};
    std::uint64_t rip = 0;
};

struct StackView {
    std::uint64_t base = 0;
    std::span<const std::byte> bytes;
};

struct UnwindProgram {
    std::uint8_t version = 0;
    std::uint8_t flags = 0;
    std::uint8_t prologue_bytes = 0;
    std::uint8_t frame_register = 0;
    std::uint8_t frame_offset = 0;
    std::vector<UnwindOperation> operations;
    std::optional<RuntimeFunction> chained;
    std::uint32_t handler_rva = 0;
    std::uint32_t handler_data_rva = 0;

    static UnwindProgram decode(std::span<const std::byte> image, const RuntimeFunction &function);
};

struct UnwindStep {
    RegisterContext context;
    std::uint64_t establisher_frame = 0;
    bool in_prologue = false;
};

// For prologue/body PCs only. The caller must first recognize epilogues and
// process chained metadata. This is not a complete RtlVirtualUnwind adapter.
UnwindStep restore_prologue(const UnwindProgram &program, const RegisterContext &context, StackView stack,
                            std::uint32_t pc_offset);
} // namespace ambrosia::pe
