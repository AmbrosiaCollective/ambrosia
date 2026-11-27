#include "native_windows.h"
#include "startup_trace.h"
#include <stdexcept>

namespace ambrosia::services {
NativeWindow::~NativeWindow() {
    if (presentation && host_window.id) {
        try {
            presentation->destroy(host_window.id);
        } catch (...) {
            // Process teardown may follow an expired broker. Do not throw from
            // ownership cleanup or claim the remote destruction succeeded.
            diagnostics::write_service_trace("TRACE native host window cleanup was not acknowledged\n");
        }
    }
}

void NativeWindows::bind(const std::filesystem::path &endpoints) {
    const std::lock_guard lock(mutex_);
    if (presentation_) {
        throw std::runtime_error("native presentation is already configured");
    }
    presentation_ = std::make_shared<host::Presentation>(endpoints);
}

std::shared_ptr<NativeWindow> NativeWindows::create(std::shared_ptr<const WindowClass> window_class,
                                                    const host::WindowRequest &request, std::uint32_t style,
                                                    std::uint32_t extended_style) {
    const std::lock_guard lock(mutex_);
    if (!presentation_ || !window_class || windows_.size() >= 32) {
        throw std::runtime_error("native host window configuration or ownership budget is unavailable");
    }
    auto window = std::make_shared<NativeWindow>();
    window->window_class = std::move(window_class);
    window->presentation = presentation_;
    window->owner = std::this_thread::get_id();
    window->title = request.title;
    window->style = style;
    window->extended_style = extended_style;
    window->extra.resize(window->window_class->description.window_extra_bytes, std::byte{0});
    window->handle = identifiers_->allocate();
    if (!window->handle) {
        throw std::runtime_error("native window handle budget is exhausted");
    }
    // A real hidden host window exists before creation callbacks. Publish the
    // guest handle before invoking any callback, and never hold this mutex there.
    window->host_window = presentation_->create(request);
    windows_.emplace(window->handle, window);
    return window;
}

std::shared_ptr<NativeWindow> NativeWindows::find(std::uintptr_t handle) const {
    const std::lock_guard lock(mutex_);
    const auto found = windows_.find(handle);
    return found == windows_.end() ? nullptr : found->second;
}

void NativeWindows::retire(std::uintptr_t handle) {
    std::shared_ptr<NativeWindow> removed;
    {
        const std::lock_guard lock(mutex_);
        const auto found = windows_.find(handle);
        if (found != windows_.end()) {
            removed = std::move(found->second);
            windows_.erase(found);
        }
    }
    // Remote cleanup is outside the registry lock and never calls guest code.
}
} // namespace ambrosia::services
