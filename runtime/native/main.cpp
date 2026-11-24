#include "native_command.h"
#include <iostream>
#include <vector>

namespace {
template <class Character> int command(int argc, Character **argv) {
    std::vector<std::filesystem::path> arguments;
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }
    return ambrosia::run_native_command(arguments, std::cout, std::cerr);
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
    return command(argc, argv);
}
#else
int main(int argc, char **argv) {
    return command(argc, argv);
}
#endif
