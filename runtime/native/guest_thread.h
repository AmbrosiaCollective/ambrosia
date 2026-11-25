// Per-thread static TLS storage. This is a minimal TLS anchor, not a full TEB.
#pragma once
#include "host.h"
#include <array>
#include <functional>
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
    class Activation;
    ThreadContext();
    ~ThreadContext();
    ThreadContext(const ThreadContext &) = delete;
    ThreadContext &operator=(const ThreadContext &) = delete;
    void install(std::size_t slot, const TlsTemplate &spec);
    Activation activate();
    std::span<std::byte> block(std::size_t slot);
    static ThreadContext *current() noexcept;
    std::uint32_t &last_error();
    std::int32_t &crt_error();
    void on_exit(std::function<void()> callback);

  private:
    void require_owner() const;

    struct Block {
        std::vector<std::byte> storage;
        std::size_t offset, size;
    };

    std::array<void *, 64> slots_{};
    std::array<std::unique_ptr<Block>, 64> blocks_{};

    struct Anchor {
        std::array<std::byte, 8> reserved_exception{};
        std::uintptr_t stack_base = 0;
        std::uintptr_t stack_limit = 0;
        std::array<std::byte, 0x18> reserved_tib{};
        void *self = nullptr;
        std::array<std::byte, 0x20> reserved_client{};
        void **tls = nullptr;
        std::array<std::byte, 8> reserved_error{};
        std::uint32_t last_error = 0;
    } anchor_;

    static_assert(offsetof(Anchor, tls) == 0x58);
    static_assert(offsetof(Anchor, stack_base) == 8);
    static_assert(offsetof(Anchor, stack_limit) == 0x10);
    static_assert(offsetof(Anchor, self) == 0x30);
    static_assert(offsetof(Anchor, last_error) == 0x68);
    std::thread::id owner_;
    std::int32_t crt_error_ = 0;
    bool sealed_ = false;
    bool exiting_ = false;
    std::vector<std::function<void()>> exit_callbacks_;
};

class ThreadContext::Activation {
  public:
    ~Activation();
    Activation(const Activation &) = delete;
    Activation &operator=(const Activation &) = delete;
    Activation(Activation &&) = delete;
    Activation &operator=(Activation &&) = delete;

  private:
    friend class ThreadContext;
    explicit Activation(ThreadContext &context);
    host::GuestSegmentScope segment_;
    ThreadContext *saved_;
};
} // namespace ambrosia::guest
