#include "guest_thread.h"
#include "native_windows.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include "text_conversion.h"
#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace ambrosia::services {
namespace {
struct CreateStructure {
    void *parameter;
    std::uintptr_t instance, menu, parent;
    std::int32_t height, width, y, x, style;
    std::uint32_t padding = 0;
    const char *title;
    std::uintptr_t class_id;
    std::uint32_t extended_style;
    std::uint32_t tail_padding = 0;
};

static_assert(sizeof(CreateStructure) == 80 && offsetof(CreateStructure, title) == 56);
static_assert(offsetof(CreateStructure, extended_style) == 72);

struct RestoreNativeError {
    int saved = errno;

    ~RestoreNativeError() {
        errno = saved;
    }
};

struct CreationRollback {
    NativeWindows &windows;
    NativeWindow &window;
    bool accepted = false;

    ~CreationRollback() {
        window.creation_structure = 0;
        if (!accepted) {
            windows.retire(window.handle);
        }
    }
};

std::optional<std::string> copy_string(const char *source, std::size_t limit) {
    std::string result;
    for (std::size_t index = 0; index <= limit; ++index) {
        if (!source[index]) {
            return result;
        }
        if (index == limit) {
            return std::nullopt;
        }
        result.push_back(source[index]);
    }
    return std::nullopt;
}

std::intptr_t send_creation(Runtime &runtime, const NativeWindow &window, std::uint32_t message,
                            void *parameter) {
    if (window.owner != std::this_thread::get_id() || !guest::ThreadContext::current()) {
        throw std::runtime_error("native creation message requires its owning active guest thread");
    }
    auto catalog = *window.window_class->description.modules;
    Scope callback_scope(runtime, &catalog);
    using Procedure =
        std::intptr_t(AMBROSIA_GUEST_ABI *)(std::uintptr_t, std::uint32_t, std::uintptr_t, std::intptr_t);
    const auto procedure = reinterpret_cast<Procedure>(window.window_class->description.procedure);
    return procedure(window.handle, message, 0, reinterpret_cast<std::intptr_t>(parameter));
}

std::uintptr_t AMBROSIA_GUEST_ABI create_window(std::uint32_t extended_style, std::uintptr_t class_id,
                                                const char *title, std::uint32_t style, std::int32_t x,
                                                std::int32_t y, std::int32_t width, std::int32_t height,
                                                std::uintptr_t parent, std::uintptr_t menu,
                                                std::uintptr_t instance, void *parameter) noexcept {
    RestoreNativeError restore;
    try {
        auto *runtime = current_runtime();
        if (!runtime || !current_modules() || !guest::ThreadContext::current()) {
            throw std::runtime_error("native window creation requires an active guest module context");
        }
        if (extended_style || parent || menu || (style & ~0x10000000u) != 0x00cf0000u) {
            throw std::runtime_error("unsupported native window parent, menu or creation style");
        }
        std::shared_ptr<const WindowClass> registered;
        if (class_id < 65536) {
            registered = runtime->window_classes().find(static_cast<std::uint16_t>(class_id), instance);
        } else {
            const auto name = copy_string(reinterpret_cast<const char *>(class_id), 256);
            if (name) {
                registered = runtime->window_classes().find(*name, instance);
            }
        }
        if (!registered) {
            set_last_error(1407); // ERROR_CANNOT_FIND_WND_CLASS.
            return 0;
        }
        if (!std::holds_alternative<std::monostate>(registered->description.menu)) {
            throw std::runtime_error("native window class menus require resource integration");
        }
        auto copied_title = title ? copy_string(title, 4096) : std::optional(std::string{});
        if (!copied_title || !unicode::utf8_to_utf16(*copied_title, true) || width <= 0 || height <= 0) {
            set_last_error(error_invalid_parameter);
            return 0;
        }
        host::WindowRequest request;
        request.title = std::move(*copied_title);
        request.geometry = {x, y, width, height};
        request.resizable = true;
        // WM_CREATE must occur before visibility, including WS_VISIBLE calls.
        auto window = runtime->windows().create(registered, request, style, extended_style);
        CreationRollback rollback{runtime->windows(), *window};
        const auto &geometry = window->host_window.geometry;
        CreateStructure creation{parameter,
                                 instance,
                                 menu,
                                 parent,
                                 height,
                                 width,
                                 geometry.y,
                                 geometry.x,
                                 static_cast<std::int32_t>(style),
                                 0,
                                 title,
                                 class_id,
                                 extended_style,
                                 0};
        window->creation_structure = reinterpret_cast<std::uintptr_t>(&creation);
        diagnostics::write_service_trace("TRACE native host window created; dispatching WM_NCCREATE\n");
        if (!send_creation(*runtime, *window, 0x81, &creation)) {
            return 0;
        }
        // FALSE wParam supplies one RECT in screen coordinates. The callback
        // owns its non-client calculation; do not invent native border metrics.
        const auto right = static_cast<std::int64_t>(geometry.x) + geometry.width;
        const auto bottom = static_cast<std::int64_t>(geometry.y) + geometry.height;
        if (right > INT32_MAX || bottom > INT32_MAX) {
            throw std::runtime_error("native creation rectangle exceeds the guest coordinate range");
        }
        std::array<std::int32_t, 4> rectangle{geometry.x, geometry.y, static_cast<std::int32_t>(right),
                                              static_cast<std::int32_t>(bottom)};
        (void)send_creation(*runtime, *window, 0x83, rectangle.data());
        window->client_rectangle = rectangle;
        if (send_creation(*runtime, *window, 1, &creation) == -1) {
            (void)send_creation(*runtime, *window, 2, nullptr);
            (void)send_creation(*runtime, *window, 0x82, nullptr);
            return 0;
        }
        if (style & 0x10000000u) {
            (void)window->presentation->show(window->host_window.id, true);
        }
        diagnostics::write_service_trace("TRACE native guest creation messages completed\n");
        rollback.accepted = true;
        return window->handle;
    } catch (const std::bad_alloc &) {
        set_last_error(error_not_enough_memory);
        return 0;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

std::intptr_t AMBROSIA_GUEST_ABI default_window(std::uintptr_t handle, std::uint32_t message,
                                                std::uintptr_t first, std::intptr_t second) noexcept {
    RestoreNativeError restore;
    try {
        auto *runtime = current_runtime();
        if (!runtime || !guest::ThreadContext::current()) {
            throw std::runtime_error("native default window procedure requires an active guest context");
        }
        auto window = runtime->windows().find(handle);
        if (!window) {
            set_last_error(1400); // ERROR_INVALID_WINDOW_HANDLE.
            return 0;
        }
        if (window->owner != std::this_thread::get_id()) {
            throw std::runtime_error("native default processing requires the owning window thread");
        }
        if (message == 0x81) {
            if (!second) {
                return 0;
            }
            if (!window->creation_structure ||
                static_cast<std::uintptr_t>(second) != window->creation_structure) {
                throw std::runtime_error("default NCCREATE requires its owned active creation structure");
            }
            const char *title;
            std::memcpy(&title, reinterpret_cast<const std::byte *>(second) + 56, sizeof(title));
            auto copied = title ? copy_string(title, 4096) : std::optional(std::string{});
            if (!copied || !unicode::utf8_to_utf16(*copied, true)) {
                throw std::runtime_error("default NCCREATE title requires a bounded valid UTF8 name");
            }
            // Initialize the actual owned host title before accepting creation.
            window->presentation->set_title(window->host_window.id, *copied);
            window->title = std::move(*copied);
            diagnostics::write_service_trace(
                "TRACE native default NCCREATE initialized owned window title\n");
            return 1;
        }
        // Numeric shape only: no game string, pointer or message payload bytes.
        throw std::runtime_error("unsupported native default window message=" + std::to_string(message) +
                                 " wparam_present=" + std::to_string(first != 0) +
                                 " lparam_present=" + std::to_string(second != 0));
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}
} // namespace

void register_window_services(ImportRegistry &registry) {
    for (const auto *module : {"user32.dll", "ext-ms-win-rtcore-ntuser-window-ansi-l1-1-0.dll"}) {
        registry.add(module, "CreateWindowExA", reinterpret_cast<std::uintptr_t>(&create_window));
        registry.add(module, "DefWindowProcA", reinterpret_cast<std::uintptr_t>(&default_window));
    }
}
} // namespace ambrosia::services
