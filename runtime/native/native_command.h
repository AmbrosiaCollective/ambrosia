#pragma once
#include <filesystem>
#include <ostream>
#include <span>

namespace ambrosia {
// Shared CLI implementation so argument handling and bootstrap behavior can be
// tested in-process under the sandbox that forbids child execution.
int run_native_command(std::span<const std::filesystem::path> arguments, std::ostream &output,
                       std::ostream &errors);
} // namespace ambrosia
