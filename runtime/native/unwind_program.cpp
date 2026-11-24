#include "unwind_program.h"
#include "pe_bytes.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace ambrosia::pe {
namespace {
bool nonvolatile_register(unsigned reg) {
    return reg == 3 || (reg >= 5 && reg <= 7) || (reg >= 12 && reg <= 15);
}

std::uint64_t add_address(std::uint64_t address, std::uint64_t offset) {
    if (offset > std::numeric_limits<std::uint64_t>::max() - address) {
        throw std::runtime_error("unwind address overflow");
    }
    return address + offset;
}

std::span<const std::byte> stack_range(StackView stack, std::uint64_t address, std::size_t bytes) {
    if (address < stack.base || address - stack.base > stack.bytes.size()) {
        throw std::runtime_error("unwind stack address is outside the owned range");
    }
    const auto offset = static_cast<std::size_t>(address - stack.base);
    detail::require_range(offset, bytes, stack.bytes.size());
    return stack.bytes.subspan(offset, bytes);
}

std::uint64_t stack_word(StackView stack, std::uint64_t address) {
    return detail::read<std::uint64_t>(stack_range(stack, address, 8), 0);
}
} // namespace

UnwindProgram UnwindProgram::decode(std::span<const std::byte> image, const RuntimeFunction &function) {
    const auto rva = function.unwind;
    if (!rva || rva % 4 || function.end < function.begin || function.end > image.size()) {
        throw std::runtime_error("invalid unwind function or header reference");
    }
    detail::require_range(rva, 4, image.size());
    UnwindProgram result;
    const auto header = detail::read<std::uint8_t>(image, rva);
    result.version = header & 7;
    result.flags = header >> 3;
    result.prologue_bytes = detail::read<std::uint8_t>(image, rva + 1);
    const auto count = detail::read<std::uint8_t>(image, rva + 2);
    const auto frame = detail::read<std::uint8_t>(image, rva + 3);
    result.frame_register = frame & 15;
    result.frame_offset = frame >> 4;
    if ((result.version != 1 && result.version != 2) || (result.flags & ~7) ||
        ((result.flags & 4) && (result.flags & 3)) ||
        (result.frame_register && !nonvolatile_register(result.frame_register)) ||
        (!result.frame_register && result.frame_offset)) {
        throw std::runtime_error("unsupported unwind version, flags or frame register");
    }
    const std::size_t codes = std::size_t(rva) + 4;
    detail::require_range(codes, std::size_t(count) * 2, image.size());
    unsigned previous_offset = result.prologue_bytes;
    bool saw_frame = false;
    for (unsigned index = 0; index < count;) {
        const auto at = codes + index * 2;
        UnwindOperation operation;
        operation.code_offset = detail::read<std::uint8_t>(image, at);
        const auto encoding = detail::read<std::uint8_t>(image, at + 1);
        operation.opcode = encoding & 15;
        operation.info = encoding >> 4;
        unsigned slots = 1;
        switch (operation.opcode) {
        case 0: // PUSH_NONVOL
        case 4: // SAVE_NONVOL
        case 5: // SAVE_NONVOL_FAR
            if (!nonvolatile_register(operation.info)) {
                throw std::runtime_error("unwind save names a volatile or stack register");
            }
            if (operation.opcode == 4) {
                slots = 2;
            } else if (operation.opcode == 5) {
                slots = 3;
            }
            break;
        case 1: // ALLOC_LARGE
            if (operation.info > 1) {
                throw std::runtime_error("invalid large unwind allocation encoding");
            }
            slots = operation.info ? 3 : 2;
            break;
        case 2: // ALLOC_SMALL
            operation.operand = operation.info * 8 + 8;
            break;
        case 3: // SET_FPREG
            if (!result.frame_register || operation.info || saw_frame) {
                throw std::runtime_error("invalid unwind frame-register operation");
            }
            saw_frame = true;
            break;
        case 8: // SAVE_XMM128
        case 9: // SAVE_XMM128_FAR
            if (operation.info < 6) {
                throw std::runtime_error("unwind save names a volatile XMM register");
            }
            slots = operation.opcode == 8 ? 2 : 3;
            break;
        case 10: // PUSH_MACHFRAME
            if (operation.info > 1 || index + 1 != count || (result.flags & 4)) {
                throw std::runtime_error("invalid unwind machine-frame operation");
            }
            break;
        default:
            // Version-two epilogue descriptions need the epilogue decoder.
            throw std::runtime_error("unsupported unwind operation " + std::to_string(operation.opcode));
        }
        if (slots > count - index || operation.code_offset > previous_offset) {
            throw std::runtime_error("truncated or unordered unwind operations");
        }
        if (slots == 2) {
            operation.operand = detail::read<std::uint16_t>(image, at + 2);
            operation.operand *= operation.opcode == 8 ? 16 : 8;
        } else if (slots == 3) {
            operation.operand = detail::read<std::uint32_t>(image, at + 2);
        }
        if (operation.opcode == 1 && (!operation.operand || operation.operand % 8)) {
            throw std::runtime_error("invalid unwind stack allocation size");
        }
        previous_offset = operation.code_offset;
        result.operations.push_back(operation);
        index += slots;
    }
    const auto tail = codes + ((std::size_t(count) + 1) & ~std::size_t{1}) * 2;
    if (result.flags & 4) {
        detail::require_range(tail, 12, image.size());
        RuntimeFunction chained{detail::read<std::uint32_t>(image, tail),
                                detail::read<std::uint32_t>(image, tail + 4),
                                detail::read<std::uint32_t>(image, tail + 8)};
        if (chained.end < chained.begin || chained.end > image.size() || !chained.unwind ||
            chained.unwind % 4) {
            throw std::runtime_error("invalid chained unwind function");
        }
        detail::require_range(chained.unwind, 4, image.size());
        result.chained = chained;
    } else if (result.flags & 3) {
        result.handler_rva = detail::read<std::uint32_t>(image, tail);
        if (!result.handler_rva || result.handler_rva >= image.size()) {
            throw std::runtime_error("unwind handler is outside image");
        }
        result.handler_data_rva = static_cast<std::uint32_t>(tail + 4);
    }
    return result;
}

UnwindStep restore_prologue(const UnwindProgram &program, const RegisterContext &context, StackView stack,
                            std::uint32_t pc_offset) {
    if (program.chained) {
        throw std::runtime_error("chained frame restoration needs the chain walker");
    }
    if (program.frame_register >= context.integer.size()) {
        throw std::runtime_error("invalid decoded unwind frame register");
    }
    UnwindStep result{context, context.integer[4], pc_offset < program.prologue_bytes};
    auto &registers = result.context.integer;
    const bool frame_established =
        !result.in_prologue ||
        std::any_of(program.operations.begin(), program.operations.end(), [&](const auto &operation) {
            return operation.opcode == 3 && operation.code_offset <= pc_offset;
        });
    if (program.frame_register && frame_established) {
        const auto adjustment = std::uint64_t(program.frame_offset) * 16;
        if (registers[program.frame_register] < adjustment) {
            throw std::runtime_error("unwind frame address underflow");
        }
        result.establisher_frame = registers[program.frame_register] - adjustment;
    }
    const auto frame = result.establisher_frame;
    bool machine_frame = false;
    for (const auto &operation : program.operations) {
        if (operation.info >= registers.size()) {
            throw std::runtime_error("invalid decoded unwind register");
        }
        if (operation.code_offset > pc_offset) {
            continue;
        }
        switch (operation.opcode) {
        case 0:
            registers[operation.info] = stack_word(stack, registers[4]);
            registers[4] = add_address(registers[4], 8);
            break;
        case 1:
        case 2:
            registers[4] = add_address(registers[4], operation.operand);
            break;
        case 3:
            registers[4] = frame;
            break;
        case 4:
        case 5:
            registers[operation.info] = stack_word(stack, add_address(frame, operation.operand));
            break;
        case 8:
        case 9: {
            const auto bytes = stack_range(stack, add_address(frame, operation.operand), 16);
            std::copy(bytes.begin(), bytes.end(), result.context.xmm[operation.info].begin());
            break;
        }
        case 10: {
            const auto saved = add_address(registers[4], operation.info * 8);
            result.context.rip = stack_word(stack, saved);
            registers[4] = stack_word(stack, add_address(saved, 24));
            (void)stack_range(stack, registers[4], 0);
            machine_frame = true;
            break;
        }
        default:
            throw std::runtime_error("unwind program contains an unsupported operation");
        }
    }
    if (!machine_frame) {
        result.context.rip = stack_word(stack, registers[4]);
        registers[4] = add_address(registers[4], 8);
    }
    (void)stack_range(stack, registers[4], 0);
    return result;
}
} // namespace ambrosia::pe
