#pragma once
#include "handle_ids.h"
#include "import_registry.h"
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace ambrosia::services {
class Runtime;
enum class AppNotification { state, constrained };
using AppChangeRoutine = void(AMBROSIA_GUEST_ABI *)(std::uint8_t value, void *context);

// Adapted from OpenScarlett's MIT PLM registry. See LICENSE.OpenScarlett.
// No Windows watcher, process-global registry or host event source is imported.
class AppLifecycle {
  public:
    static constexpr unsigned registration_limit = 128;

    explicit AppLifecycle(std::shared_ptr<HandleIds> ids) : ids_(std::move(ids)) {}

    std::uint32_t add(AppNotification kind, AppChangeRoutine routine, void *context, void *output);
    void remove(AppNotification kind, std::uintptr_t registration);
    unsigned count(AppNotification kind) const;

  private:
    friend class Runtime;
    // Runtime validates active guest TLS/scope. Its execution owner must retain
    // all callback-providing modules until notification returns.
    unsigned notify(AppNotification kind, bool value);

    struct Entry {
        AppNotification kind;
        AppChangeRoutine routine;
        void *context;
        bool enabled = true;
        unsigned active = 0;
        std::thread::id delivering;
    };

    void finish(const std::shared_ptr<Entry> &entry);
    std::shared_ptr<HandleIds> ids_;
    mutable std::mutex mutex_;
    std::condition_variable idle_;
    std::recursive_mutex delivery_mutex_;
    unsigned delivery_depth_ = 0;
    std::map<std::uintptr_t, std::shared_ptr<Entry>> entries_;
};

void register_app_lifecycle(ImportRegistry &registry);
} // namespace ambrosia::services
