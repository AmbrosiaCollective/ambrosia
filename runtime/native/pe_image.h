// Host-independent, fail-closed PE32+ bootstrap. TLS/DLL/SEH support comes later.
#pragma once
#include "host.h"
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#if defined(_M_X64)
#define AMBROSIA_GUEST_ABI
#elif defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#define AMBROSIA_GUEST_ABI __attribute__((ms_abi))
#else
#error Native guest execution currently requires an x86-64 host and Microsoft x64 ABI support
#endif

namespace ambrosia::pe {
struct Import {
    std::string module;
    std::string name;
    std::uint16_t ordinal = 0;
    bool by_ordinal = false;
};
// Only explicit adapters may resolve imports. Never fall back to host dlopen.
using Resolver = std::function<std::uintptr_t(const Import&)>;

class Image {
public:
    static Image load(std::span<const std::byte> bytes, const Resolver& resolver);
    Image(Image&&) noexcept = default;
    Image& operator=(Image&&) noexcept = default;
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
    // Bootstrap ABI only: no arguments, uint64 result; not a retail entry contract.
    std::uint64_t run_bootstrap() const;
    const std::byte* base() const { return memory_.data(); }
    std::size_t size() const { return memory_.size(); }
    const std::vector<Import>& imports() const { return imports_; }
private:
    explicit Image(std::size_t size) : memory_(size) {}
    host::Mapping memory_;
    std::uint32_t entry_ = 0;
    std::vector<Import> imports_;
};
} // namespace ambrosia::pe
