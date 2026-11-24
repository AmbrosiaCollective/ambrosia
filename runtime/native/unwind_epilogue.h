#pragma once
#include "unwind_program.h"

namespace ambrosia::pe {
// Recognize the complete remaining epilogue before touching even the copied
// register context. A non-match leaves body/prologue restoration to the caller.
std::optional<UnwindStep> restore_epilogue(std::span<const std::byte> image, const RuntimeFunction &function,
                                           const UnwindProgram &program, std::uint32_t pc_rva,
                                           const RegisterContext &context, StackView stack);
} // namespace ambrosia::pe
