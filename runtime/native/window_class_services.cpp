#include "guest_strings.h"
#include "guest_thread.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include "window_classes.h"
#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace ambrosia::services {
namespace {
struct ClassInput {
    std::uint32_t size, style;
    std::uint64_t procedure;
    std::int32_t class_extra, window_extra;
    std::uint64_t instance, icon, cursor, background, menu, name, small_icon;
};

static_assert(sizeof(ClassInput) == 80);
static_assert(offsetof(ClassInput, instance) == 24 && offsetof(ClassInput, name) == 64);

struct PreserveNativeError {
    int saved = errno;

    ~PreserveNativeError() {
        errno = saved;
    }
};

std::uint16_t failure(std::uint32_t error) {
    set_last_error(error);
    return 0;
}

std::optional<std::string> class_name(const char *input) {
    std::string value;
    for (std::size_t index = 0; index <= 256; ++index) {
        if (!input[index]) {
            return value.empty() ? std::nullopt : std::optional(std::move(value));
        }
        if (index == 256) {
            return std::nullopt;
        }
        value.push_back(input[index]);
    }
    return std::nullopt;
}

std::uint16_t AMBROSIA_GUEST_ABI register_class(const void *input) noexcept {
    PreserveNativeError restore;
    try {
        auto *runtime = current_runtime();
        const auto *graph = current_modules();
        if (!runtime || !graph || !guest::ThreadContext::current()) {
            throw std::runtime_error("window-class registration requires an active guest module context");
        }
        if (!input) {
            return failure(998); // ERROR_NOACCESS.
        }
        std::uint32_t size;
        std::memcpy(&size, input, sizeof(size));
        if (size != sizeof(ClassInput)) {
            return failure(error_invalid_parameter);
        }
        ClassInput attributes;
        std::memcpy(&attributes, input, sizeof(attributes));
        if (!attributes.procedure || !attributes.name || attributes.class_extra < 0 ||
            attributes.window_extra < 0 || attributes.class_extra > 40 || attributes.window_extra > 40) {
            return failure(error_invalid_parameter);
        }
        const auto procedure_module = graph->module_base_for_address(attributes.procedure);
        if (!procedure_module || (attributes.instance && graph->module_base_for_address(
                                                             attributes.instance) != attributes.instance)) {
            throw std::runtime_error(
                "window class procedure or instance is outside its mapped guest catalog");
        }
        WindowClassDescription description;
        if (attributes.name < 65536) {
            const auto registered = runtime->window_classes().find(
                static_cast<std::uint16_t>(attributes.name), attributes.instance);
            if (!registered) {
                return failure(error_invalid_parameter);
            }
            description.name = registered->description.name;
        } else {
            auto name = class_name(reinterpret_cast<const char *>(attributes.name));
            if (!name) {
                return failure(error_invalid_parameter);
            }
            description.name = std::move(*name);
        }
        if (attributes.menu && attributes.menu < 65536) {
            description.menu = static_cast<std::uint16_t>(attributes.menu);
        } else if (attributes.menu) {
            description.menu = guest::read_string(reinterpret_cast<const char *>(attributes.menu));
        }
        description.style = attributes.style;
        description.procedure = attributes.procedure;
        description.instance = attributes.instance;
        description.icon = attributes.icon;
        description.cursor = attributes.cursor;
        description.background = attributes.background;
        description.small_icon = attributes.small_icon;
        description.window_extra_bytes = static_cast<std::uint32_t>(attributes.window_extra);
        description.class_extra.resize(static_cast<std::size_t>(attributes.class_extra), std::byte{0});
        description.modules = std::make_shared<pe::ModuleGraph>(*graph);
        const auto result = runtime->window_classes().add(std::move(description));
        if (result.error) {
            return failure(result.error);
        }
        diagnostics::write_service_trace("TRACE native window class registered\n");
        return result.atom;
    } catch (const std::bad_alloc &) {
        return failure(error_not_enough_memory);
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}
} // namespace

void register_window_class_services(ImportRegistry &registry) {
    for (const auto *module : {"user32.dll", "ext-ms-win-rtcore-ntuser-window-ansi-l1-1-0.dll",
                               "ext-ms-win-ntuser-windowclass-l1-1-0.dll"}) {
        registry.add(module, "RegisterClassExA", reinterpret_cast<std::uintptr_t>(&register_class));
    }
}
} // namespace ambrosia::services
