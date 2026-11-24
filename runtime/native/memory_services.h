#pragma once
#include "host.h"
#include <map>
#include <memory>
#include <vector>

namespace ambrosia::services {
inline constexpr std::uint32_t error_invalid_function = 1;
inline constexpr std::uint32_t error_not_enough_memory = 8;
inline constexpr std::uint32_t error_not_supported = 50;
inline constexpr std::uint32_t error_invalid_parameter = 87;
inline constexpr std::uint32_t error_invalid_address = 487;

struct AllocationResult {
    void *address = nullptr;
    std::uint32_t error = 0;
};

struct FreeResult {
    bool success = false;
    std::uint32_t error = 0;
};

class MemoryServices {
  public:
    explicit MemoryServices(std::size_t budget = 64 * 1024 * 1024) : budget_(budget) {}

    MemoryServices(const MemoryServices &) = delete;
    MemoryServices &operator=(const MemoryServices &) = delete;
    MemoryServices(MemoryServices &&) = delete;
    MemoryServices &operator=(MemoryServices &&) = delete;

    AllocationResult allocate(void *address, std::size_t bytes, std::uint32_t flags,
                              std::uint32_t protection) noexcept;
    FreeResult free(void *address, std::size_t bytes, std::uint32_t flags) noexcept;

    std::size_t reserved_bytes() const {
        return reserved_;
    }

  private:
    struct Region {
        host::Mapping memory;
        std::vector<std::uint32_t> protection;

        explicit Region(std::size_t size)
            : memory(size, host::Access::none, 65536), protection(size / host::Mapping::page_size(), 0) {}
    };

    std::map<std::uintptr_t, std::unique_ptr<Region>> regions_;
    std::size_t budget_;
    std::size_t reserved_ = 0;
};
} // namespace ambrosia::services
