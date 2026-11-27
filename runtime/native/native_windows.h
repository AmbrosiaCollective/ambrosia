#pragma once
#include "handle_ids.h"
#include "host_presentation.h"
#include "window_classes.h"
#include <array>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace ambrosia::services {
struct NativeWindow {
    ~NativeWindow();
    std::uintptr_t handle = 0;
    std::shared_ptr<const WindowClass> window_class;
    std::shared_ptr<host::Presentation> presentation;
    host::PresentationWindow host_window;
    std::thread::id owner;
    std::uint32_t style = 0, extended_style = 0;
    // Nonzero only while the owning thread is dispatching creation messages.
    std::uintptr_t creation_structure = 0;
    std::uintptr_t calculation_structure = 0;
    host::WindowBorders borders;
    bool visible = false;
    std::optional<bool> pending_visibility;
    std::string title;
    std::array<std::int32_t, 4> client_rectangle{};
    std::vector<std::byte> extra;
};

std::array<std::int32_t, 4> calculate_client_rectangle(const std::array<std::int32_t, 4> &outer,
                                                       const host::WindowBorders &borders);

std::int32_t resolve_show_command(std::int32_t command, std::optional<std::uint16_t> startup, bool first);

class NativeWindows {
  public:
    explicit NativeWindows(std::shared_ptr<HandleIds> identifiers) : identifiers_(std::move(identifiers)) {}

    void bind(const std::filesystem::path &endpoints);
    std::shared_ptr<NativeWindow> create(std::shared_ptr<const WindowClass> window_class,
                                         const host::WindowRequest &request, std::uint32_t style,
                                         std::uint32_t extended_style);
    std::shared_ptr<NativeWindow> find(std::uintptr_t handle) const;
    void retire(std::uintptr_t handle);
    bool consume_startup_show();

  private:
    std::shared_ptr<HandleIds> identifiers_;
    mutable std::mutex mutex_;
    std::shared_ptr<host::Presentation> presentation_;
    std::map<std::uintptr_t, std::shared_ptr<NativeWindow>> windows_;
    bool startup_show_consumed_ = false;
};

void register_window_services(ImportRegistry &registry);
} // namespace ambrosia::services
