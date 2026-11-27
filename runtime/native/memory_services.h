#pragma once
#include "host.h"
#include "physical_span.h"
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace ambrosia::services {
class PhysicalPage;
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

struct PhysicalSpanResult {
    std::optional<PhysicalSpan> span;
    std::uint32_t error = 0;
};

enum class XmemBacking { private_pages, physical_window, placeholder };

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
    AllocationResult reserve_placeholder(std::size_t bytes, std::uint64_t attributes) noexcept;
    AllocationResult replace_placeholder_window(void *address, std::size_t bytes, std::uint64_t attributes,
                                                std::uint32_t protection) noexcept;
    AllocationResult allocate_xmem(std::size_t bytes, std::uint64_t attributes, std::uint32_t protection,
                                   std::size_t guest_page_bytes) noexcept;
    std::optional<XmemRegionInfo> xmem_region(const void *address) const;
    std::optional<XmemRegionInfo> window(const void *address) const;
    std::uint32_t map_physical(void *address,
                               std::span<const std::shared_ptr<const PhysicalPage>> pages) noexcept;
    PhysicalSpanResult physical_span(const void *address, std::size_t bytes) const noexcept;

    std::size_t reserved_bytes() const {
        const std::lock_guard lock(mutex_);
        return reserved_;
    }

  private:
    AllocationResult reserve_region(std::size_t bytes, std::uint32_t flags, std::uint32_t protection,
                                    std::optional<XmemRegionInfo> xmem);

    struct Region {
        // Declared before Mapping so mapped views are destroyed before leases.
        std::map<std::size_t, std::shared_ptr<const PhysicalPage>> physical_views;
        host::Mapping memory;
        std::vector<std::uint32_t> protection;
        std::optional<XmemRegionInfo> xmem;
        // Child metadata never owns the underlying mapping or adds to its budget.
        std::map<std::uintptr_t, XmemRegionInfo> placeholder_windows;
        std::map<std::uintptr_t, std::size_t> placeholder_segments;

        explicit Region(std::size_t size, std::size_t alignment)
            : memory(size, host::Access::none, alignment), protection(size / host::Mapping::page_size(), 0) {}

        bool is_sparse_reservation() const {
            return xmem && xmem->backing != XmemBacking::private_pages;
        }

        std::optional<XmemRegionInfo> window_at(std::uintptr_t pointer) const;
    };

    std::map<std::uintptr_t, std::unique_ptr<Region>> regions_;
    mutable std::mutex mutex_;
    MemoryLimits limits_;
    std::size_t reserved_ = 0;
    std::size_t sparse_reserved_ = 0;
};
} // namespace ambrosia::services
