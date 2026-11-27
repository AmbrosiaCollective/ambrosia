#pragma once
#include "host_shared_backing.h"
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace ambrosia::services {
inline constexpr std::size_t physical_page_bytes = 65536;
inline constexpr std::uint64_t physical_slot_limit = std::uint64_t{1} << 21;

class PhysicalPage {
  public:
    ~PhysicalPage();
    PhysicalPage(const PhysicalPage &) = delete;
    PhysicalPage &operator=(const PhysicalPage &) = delete;
    // Retain this lease for the entire lifetime of every mapped alias.
    void map_into(host::Mapping &destination, std::size_t offset, host::Access access) const;

  private:
    friend class PhysicalPages;
    struct State;

    explicit PhysicalPage(std::shared_ptr<State> state) : state_(std::move(state)) {}

    std::shared_ptr<State> state_;
    std::size_t offset_ = SIZE_MAX;
};

struct PhysicalAllocation {
    std::vector<std::uint64_t> pages;
    std::uint32_t error = 0;
};

class PhysicalPages {
  public:
    static constexpr std::size_t maximum_backing_bytes = physical_page_bytes * (physical_slot_limit - 1);
    void bind(host::SharedBacking backing);
    PhysicalAllocation allocate(std::uint64_t attributes, std::uint64_t count) noexcept;
    // Like the inherited API, an invalid token stops after freeing its prefix.
    std::uint32_t free(std::span<const std::uint64_t> pages) noexcept;
    std::shared_ptr<const PhysicalPage> lease(std::uint64_t page) const;
    std::size_t capacity() const;
    std::size_t available() const;

  private:
    mutable std::mutex mutex_;
    std::shared_ptr<PhysicalPage::State> backing_;
    std::map<std::uint64_t, std::shared_ptr<PhysicalPage>> pages_;
    std::vector<std::uint64_t> free_slots_;
    std::uint64_t next_slot_ = 1;
};
} // namespace ambrosia::services
