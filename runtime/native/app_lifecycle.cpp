#include "app_lifecycle.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace ambrosia::services {
std::uint32_t AppLifecycle::add(AppNotification kind, AppChangeRoutine routine, void *context, void *output) {
    if (!output) {
        return 87;
    }
    std::uintptr_t handle = 0;
    std::memcpy(output, &handle, sizeof(handle));
    if (!routine) {
        return 87;
    }
    std::lock_guard lock(mutex_);
    if (entries_.size() >= registration_limit) {
        return 8;
    }
    auto entry = std::make_shared<Entry>();
    entry->kind = kind;
    entry->routine = routine;
    entry->context = context;
    handle = ids_->allocate();
    if (!handle) {
        throw std::runtime_error("application notification IDs exhausted");
    }
    entries_.emplace(handle, std::move(entry));
    std::memcpy(output, &handle, sizeof(handle));
    return 0;
}

void AppLifecycle::remove(AppNotification kind, std::uintptr_t registration) {
    std::unique_lock lock(mutex_);
    const auto found = entries_.find(registration);
    if (found == entries_.end() || found->second->kind != kind) {
        return;
    }
    const auto entry = found->second;
    entry->enabled = false;
    entries_.erase(found);
    // The callback may unregister itself. The delivery snapshot retains its
    // entry until return; other callers wait so they can safely free context.
    idle_.wait(lock, [&] { return !entry->active || entry->delivering == std::this_thread::get_id(); });
}

unsigned AppLifecycle::count(AppNotification kind) const {
    std::lock_guard lock(mutex_);
    return static_cast<unsigned>(std::count_if(
        entries_.begin(), entries_.end(), [kind](const auto &item) { return item.second->kind == kind; }));
}

void AppLifecycle::finish(const std::shared_ptr<Entry> &entry) {
    std::lock_guard lock(mutex_);
    if (!--entry->active) {
        entry->delivering = {};
    }
    idle_.notify_all();
}

unsigned AppLifecycle::notify(AppNotification kind, bool value) {
    // Serialize batches while permitting callbacks to register/unregister and
    // issue a bounded nested notification on this same guest thread.
    std::lock_guard delivery(delivery_mutex_);
    if (delivery_depth_ >= 8) {
        throw std::runtime_error("application notification recursion budget exceeded");
    }

    struct DeliveryDepth {
        unsigned &depth;

        explicit DeliveryDepth(unsigned &value) : depth(value) {
            ++depth;
        }

        ~DeliveryDepth() {
            --depth;
        }
    } depth(delivery_depth_);

    std::vector<std::shared_ptr<Entry>> snapshot;
    {
        std::lock_guard lock(mutex_);
        for (const auto &[handle, entry] : entries_) {
            (void)handle;
            if (entry->kind == kind) {
                snapshot.push_back(entry);
            }
        }
    }
    unsigned delivered = 0;
    for (const auto &entry : snapshot) {
        {
            std::lock_guard lock(mutex_);
            if (!entry->enabled) {
                continue;
            }
            ++entry->active;
            entry->delivering = std::this_thread::get_id();
        }
        try {
            entry->routine(static_cast<std::uint8_t>(value), entry->context);
        } catch (...) {
            finish(entry);
            throw;
        }
        finish(entry);
        ++delivered;
    }
    return delivered;
}
} // namespace ambrosia::services
