#include "unwind_epilogue.h"
#include "pe_bytes.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace ambrosia::pe {
namespace {
struct Epilogue {
    std::optional<unsigned> frame_register;
    std::int64_t adjustment = 0;
    std::vector<unsigned> pops;
    unsigned return_bytes = 0;
};

bool nonvolatile(unsigned reg) {
    return reg == 3 || (reg >= 5 && reg <= 7) || (reg >= 12 && reg <= 15);
}

std::optional<Epilogue> recognize(std::span<const std::byte> code, std::uint32_t pc_rva,
                                  const RuntimeFunction &function, unsigned frame_register) {
    Epilogue result;
    std::size_t cursor = 0;
    if (code.size() >= 4 && code[0] == std::byte{0x48} && code[1] == std::byte{0x83} &&
        code[2] == std::byte{0xc4}) {
        result.adjustment = static_cast<std::int8_t>(detail::read<std::uint8_t>(code, 3));
        if (result.adjustment < 0) {
            return std::nullopt;
        }
        cursor = 4;
    } else if (code.size() >= 7 && code[0] == std::byte{0x48} && code[1] == std::byte{0x81} &&
               code[2] == std::byte{0xc4}) {
        result.adjustment = static_cast<std::int32_t>(detail::read<std::uint32_t>(code, 3));
        if (result.adjustment < 0) {
            return std::nullopt;
        }
        cursor = 7;
    } else if (code.size() >= 4 && (code[0] == std::byte{0x48} || code[0] == std::byte{0x49}) &&
               code[1] == std::byte{0x8d}) {
        const auto modrm = detail::read<std::uint8_t>(code, 2);
        const auto mode = modrm >> 6;
        const unsigned reg = (modrm & 7) + (code[0] == std::byte{0x49} ? 8 : 0);
        if (((modrm >> 3) & 7) != 4 || !frame_register || reg != frame_register) {
            return std::nullopt;
        }
        std::size_t displacement_at = 3;
        if ((modrm & 7) == 4) {
            if (code[3] != std::byte{0x24}) {
                return std::nullopt;
            }
            displacement_at = 4;
        }
        result.frame_register = reg;
        if (mode == 1 && code.size() > displacement_at) {
            result.adjustment = static_cast<std::int8_t>(detail::read<std::uint8_t>(code, displacement_at));
            cursor = displacement_at + 1;
        } else if (mode == 2 && code.size() - displacement_at >= 4) {
            result.adjustment = static_cast<std::int32_t>(detail::read<std::uint32_t>(code, displacement_at));
            cursor = displacement_at + 4;
        } else {
            return std::nullopt;
        }
    }
    for (unsigned count = 0; count < 32 && cursor < code.size(); ++count) {
        unsigned extension = 0;
        const auto prefix = detail::read<std::uint8_t>(code, cursor);
        if ((prefix & 0xf0) == 0x40) {
            extension = (prefix & 1) * 8;
            if (++cursor == code.size()) {
                return std::nullopt;
            }
        }
        const auto opcode = detail::read<std::uint8_t>(code, cursor);
        if (opcode >= 0x58 && opcode <= 0x5f) {
            const auto reg = opcode - 0x58 + extension;
            if (!nonvolatile(reg) ||
                std::find(result.pops.begin(), result.pops.end(), reg) != result.pops.end()) {
                return std::nullopt;
            }
            result.pops.push_back(reg);
            ++cursor;
            continue;
        }
        if (extension) {
            return std::nullopt;
        }
        if (opcode == 0xc3) {
            return result;
        }
        if (opcode == 0xc2 && code.size() - cursor >= 3) {
            result.return_bytes = detail::read<std::uint16_t>(code, cursor + 1);
            return result;
        }
        if (opcode == 0xf3 && code.size() - cursor >= 2 && code[cursor + 1] == std::byte{0xc3}) {
            return result;
        }
        if ((opcode == 0xe9 && code.size() - cursor >= 5) || (opcode == 0xeb && code.size() - cursor >= 2)) {
            const auto length = opcode == 0xe9 ? 5u : 2u;
            const std::int64_t displacement =
                opcode == 0xe9 ? static_cast<std::int32_t>(detail::read<std::uint32_t>(code, cursor + 1))
                               : static_cast<std::int8_t>(detail::read<std::uint8_t>(code, cursor + 1));
            const auto target =
                std::int64_t(pc_rva) + static_cast<std::int64_t>(cursor) + length + displacement;
            if (target < function.begin || target >= function.end) {
                return result;
            }
        }
        // Conservative RIP-relative indirect tail jump. The target is not read
        // or invoked; unwinding returns to the caller of the current frame.
        if (opcode == 0xff && code.size() - cursor >= 6 && code[cursor + 1] == std::byte{0x25}) {
            return result;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

std::uint64_t adjusted(std::uint64_t value, std::int64_t offset) {
    if (offset < 0) {
        const auto magnitude = static_cast<std::uint64_t>(-offset);
        if (value < magnitude) {
            throw std::runtime_error("epilogue stack address underflow");
        }
        return value - magnitude;
    }
    if (std::uint64_t(offset) > std::numeric_limits<std::uint64_t>::max() - value) {
        throw std::runtime_error("epilogue stack address overflow");
    }
    return value + offset;
}
} // namespace

std::optional<UnwindStep> restore_epilogue(std::span<const std::byte> image, const RuntimeFunction &function,
                                           const UnwindProgram &program, std::uint32_t pc_rva,
                                           const RegisterContext &context, StackView stack) {
    if (function.end > image.size() || pc_rva < function.begin || pc_rva >= function.end) {
        throw std::runtime_error("epilogue PC is outside function");
    }
    if (pc_rva - function.begin < program.prologue_bytes) {
        return std::nullopt;
    }
    const auto code = image.subspan(pc_rva, function.end - pc_rva);
    const auto decoded = recognize(code, pc_rva, function, program.frame_register);
    if (!decoded) {
        return std::nullopt;
    }
    UnwindStep result{context, context.integer[4], false};
    auto &registers = result.context.integer;
    const auto base = decoded->frame_register ? registers[*decoded->frame_register] : registers[4];
    registers[4] = adjusted(base, decoded->adjustment);
    auto pop = [&]() {
        if (registers[4] < stack.base || registers[4] - stack.base > stack.bytes.size()) {
            throw std::runtime_error("epilogue pop is outside owned stack");
        }
        const auto value = detail::read<std::uint64_t>(stack.bytes, registers[4] - stack.base);
        registers[4] = adjusted(registers[4], 8);
        return value;
    };
    for (auto reg : decoded->pops) {
        registers[reg] = pop();
    }
    result.context.rip = pop();
    registers[4] = adjusted(registers[4], decoded->return_bytes);
    if (registers[4] < stack.base || registers[4] - stack.base > stack.bytes.size()) {
        throw std::runtime_error("epilogue caller stack is outside owned extent");
    }
    return result;
}
} // namespace ambrosia::pe
