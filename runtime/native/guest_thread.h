// Per-thread static TLS storage. This is a minimal TLS anchor, not a full TEB.
#pragma once
#include "host.h"
#include <array>
#include <memory>
#include <span>
#include <thread>
#include <vector>

namespace ambrosia::guest {
struct TlsTemplate {
    std::vector<std::byte> initial;
    std::size_t zero_fill = 0;
    std::size_t alignment = 16;
};

class ThreadContext {
  public:
    ThreadContext();
    ThreadContext(const ThreadContext &) = delete;
    ThreadContext &operator=(const ThreadContext &) = delete;
    void install(std::size_t slot, const TlsTemplate &spec);
    host::GuestSegmentScope activate();
    std::span<std::byte> block(std::size_t slot);

  private:
    void require_owner() const;

    struct Block {
        std::vector<std::byte> storage;
        std::size_t offset, size;
    };

    std::array<void *, 64> slots_{};
    std::array<std::unique_ptr<Block>, 64> blocks_{};

    struct Anchor {
        std::array<std::byte, 0x58> reserved{};
        void **tls = nullptr;
    } anchor_;

    static_assert(offsetof(Anchor, tls) == 0x58);
    std::thread::id owner_;
    bool sealed_ = false;
};
} // namespace ambrosia::guest
