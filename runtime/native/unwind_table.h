// Portable x64 runtime-function metadata. No host exception registration here.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ambrosia::pe {
struct RuntimeFunction {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    std::uint32_t unwind = 0;
};

static_assert(sizeof(RuntimeFunction) == 12);
static_assert(offsetof(RuntimeFunction, unwind) == 8);

class UnwindTable {
  public:
    static UnwindTable parse(std::span<const std::byte> bytes, std::uint32_t image_bytes);
    const RuntimeFunction *find(std::uint32_t pc_rva) const;

    std::span<const RuntimeFunction> entries() const {
        return entries_;
    }

  private:
    std::vector<RuntimeFunction> entries_;
};
} // namespace ambrosia::pe
