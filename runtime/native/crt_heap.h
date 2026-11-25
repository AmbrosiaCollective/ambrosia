#pragma once
#include "import_registry.h"
#include <map>
#include <mutex>

namespace ambrosia::services {
enum class HeapError { none, exhausted, foreign_pointer };

struct HeapResult {
    void *address = nullptr;
    HeapError error = HeapError::none;
};

class CrtHeap {
  public:
    explicit CrtHeap(std::size_t budget = 64 * 1024 * 1024) : budget_(budget) {}

    ~CrtHeap();
    CrtHeap(const CrtHeap &) = delete;
    CrtHeap &operator=(const CrtHeap &) = delete;
    HeapResult allocate(std::size_t bytes, bool zero = false) noexcept;
    HeapResult resize(void *address, std::size_t bytes) noexcept;
    bool release(void *address) noexcept;

  private:
    std::mutex mutex_;
    std::map<void *, std::size_t> allocations_;
    std::size_t budget_;
    std::size_t used_ = 0;
};

void register_crt_heap(ImportRegistry &registry);
} // namespace ambrosia::services
