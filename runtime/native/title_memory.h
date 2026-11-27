#pragma once
#include <cstddef>
#include <cstdint>

namespace ambrosia::pe {
class ModuleGraph;
}

namespace ambrosia::services {
class Runtime;
inline constexpr std::uint64_t descriptor_memory_attributes = 0x00800004;

// A title allocation must be explicitly released inside its original runtime,
// module generation and active guest context. Destruction never invokes guest
// code implicitly during process/module teardown.
class TitleMemoryAllocation {
  public:
    TitleMemoryAllocation(const TitleMemoryAllocation &) = delete;
    TitleMemoryAllocation &operator=(const TitleMemoryAllocation &) = delete;
    TitleMemoryAllocation(TitleMemoryAllocation &&other) noexcept;
    TitleMemoryAllocation &operator=(TitleMemoryAllocation &&) = delete;
    void *data() const;
    std::size_t size() const;
    void close();

  private:
    friend TitleMemoryAllocation allocate_title_descriptors(std::size_t bytes);
    TitleMemoryAllocation() = default;
    Runtime *runtime_ = nullptr;
    pe::ModuleGraph *modules_ = nullptr;
    std::uint64_t generation_ = 0;
    std::uintptr_t free_ = 0;
    void *data_ = nullptr;
    std::size_t bytes_ = 0;
};

TitleMemoryAllocation allocate_title_descriptors(std::size_t bytes);
} // namespace ambrosia::services
