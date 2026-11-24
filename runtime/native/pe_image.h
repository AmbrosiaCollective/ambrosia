// Host-independent PE32+ bootstrap. Full services and guest SEH remain unsupported.
#pragma once
#include "guest_thread.h"
#include "host.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
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
using Resolver = std::function<std::uintptr_t(const Import &)>;

class Image {
    struct State;

  public:
    class Execution;
    static Image load(std::span<const std::byte> bytes, const Resolver &resolver);
    Image(Image &&) noexcept = default;
    Image &operator=(Image &&) noexcept = default;
    Image(const Image &) = delete;
    Image &operator=(const Image &) = delete;
    // Bootstrap ABI only: no arguments, uint64 result; not a retail entry contract.
    std::uint64_t run_bootstrap() const;
    Execution start() const;
    const std::byte *base() const;
    std::size_t size() const;
    const std::vector<Import> &imports() const;

  private:
    explicit Image(std::size_t size);
    std::shared_ptr<State> state_;
};

// One dynamic module lifetime on its creating host thread. Owns the mapping
// through detach, even if the Image handle is moved or destroyed. This is not
// yet a process-wide module graph or a new OS-thread launcher.
class Image::Execution {
  public:
    ~Execution();
    Execution(const Execution &) = delete;
    Execution &operator=(const Execution &) = delete;
    Execution(Execution &&) = delete;
    Execution &operator=(Execution &&) = delete;
    std::uint64_t run_bootstrap();
    void close();
    std::span<std::byte> tls_block();

  private:
    friend class Image;
    explicit Execution(std::shared_ptr<State> state);
    void require_open() const;
    bool notify(std::uint32_t reason);
    std::shared_ptr<State> state_;
    guest::ThreadContext thread_;
    std::thread::id owner_;
    bool closed_ = true;
};
} // namespace ambrosia::pe
