#pragma once
#include "host.h"
#include <map>
#include <memory>
#include <optional>
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

enum class XmemBacking { private_pages, physical_window };

struct XmemRegionInfo {
    void *base = nullptr;
    std::size_t bytes = 0;
    std::uint64_t attributes = 0;
    std::uint32_t protection = 0;
    std::size_t guest_page_bytes = 65536;
    XmemBacking backing = XmemBacking::physical_window;
};

struct MemoryLimits {
    std::size_t private_bytes = 64 * 1024 * 1024;
    std::size_t window_bytes = 64 * 1024 * 1024;
    std::size_t total_bytes = 64 * 1024 * 1024;
};

class MemoryServices {
  public:
    explicit MemoryServices(std::size_t budget = 64 * 1024 * 1024) : limits_{budget, budget, budget} {}

    explicit MemoryServices(MemoryLimits limits) : limits_(limits) {}

    MemoryServices(const MemoryServices &) = delete;
    MemoryServices &operator=(const MemoryServices &) = delete;
    MemoryServices(MemoryServices &&) = delete;
    MemoryServices &operator=(MemoryServices &&) = delete;

    AllocationResult allocate(void *address, std::size_t bytes, std::uint32_t flags,
                              std::uint32_t protection) noexcept;
    FreeResult free(void *address, std::size_t bytes, std::uint32_t flags) noexcept;
    AllocationResult reserve_window(std::size_t bytes, std::uint64_t attributes,
                                    std::uint32_t protection) noexcept;
    AllocationResult allocate_xmem(std::size_t bytes, std::uint64_t attributes, std::uint32_t protection,
                                   std::size_t guest_page_bytes) noexcept;
    std::optional<XmemRegionInfo> xmem_region(const void *address) const;
    std::optional<XmemRegionInfo> window(const void *address) const;

    std::size_t reserved_bytes() const {
        return reserved_;
    }

  private:
    AllocationResult reserve_region(std::size_t bytes, std::uint32_t flags, std::uint32_t protection,
                                    std::optional<XmemRegionInfo> xmem);

    struct Region {
        host::Mapping memory;
        std::vector<std::uint32_t> protection;
        std::optional<XmemRegionInfo> xmem;

        explicit Region(std::size_t size, std::size_t alignment)
            : memory(size, host::Access::none, alignment), protection(size / host::Mapping::page_size(), 0) {}

        bool is_window() const {
            return xmem && xmem->backing == XmemBacking::physical_window;
        }
    };

    std::map<std::uintptr_t, std::unique_ptr<Region>> regions_;
    MemoryLimits limits_;
    std::size_t reserved_ = 0;
    std::size_t window_reserved_ = 0;
};
} // namespace ambrosia::services
