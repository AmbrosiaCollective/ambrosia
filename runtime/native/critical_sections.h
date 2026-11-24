#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>

namespace ambrosia::services {
class CriticalSections {
  public:
    CriticalSections() = default;
    ~CriticalSections();
    CriticalSections(const CriticalSections &) = delete;
    CriticalSections &operator=(const CriticalSections &) = delete;

    std::uint32_t initialize(void *address, std::uint32_t spin, std::uint32_t flags) noexcept;
    bool enter(void *address, bool try_only = false);
    void leave(void *address);
    void remove(void *address);
    std::uint32_t set_spin(void *address, std::uint32_t spin);
    std::size_t size() const;

  private:
    struct State;
    std::shared_ptr<State> find(void *address) const;
    mutable std::mutex directory_;
    std::map<void *, std::shared_ptr<State>> sections_;
};
} // namespace ambrosia::services
