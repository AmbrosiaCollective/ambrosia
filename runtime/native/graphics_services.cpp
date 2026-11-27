#include "graphics_services.h"
#include "guest_thread.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

namespace ambrosia::services {
namespace {
std::int32_t AMBROSIA_GUEST_ABI create_device(void *adapter, const void *parameters, const void *interface_id,
                                              void **output) noexcept {
    const auto saved_errno = errno;
    if (!output) {
        errno = saved_errno;
        return static_cast<std::int32_t>(0x80004003u);
    }
    *output = nullptr;
    if (!interface_id) {
        errno = saved_errno;
        return static_cast<std::int32_t>(0x80004003u);
    }
    try {
        if (!current_runtime() || !guest::ThreadContext::current()) {
            diagnostics::stop_trace("native graphics creation requires an active guest context", 78);
        }
        // The existing Xbox entry contract has four arguments. The complete
        // parameter layout is unverified; capture only its observed 96-byte prefix
        // in the explicitly enabled private trace before connecting a backend.
        std::array<unsigned char, 16> identifier;
        std::memcpy(identifier.data(), interface_id, identifier.size());
        std::string trace = "TRACE native graphics request adapter_present=";
        trace += adapter ? "1" : "0";
        trace += " interface=";
        for (const auto value : identifier) {
            std::array<char, 3> text{};
            (void)std::snprintf(text.data(), text.size(), "%02x", static_cast<unsigned>(value));
            trace.append(text.data(), 2);
        }
        trace += " parameters_present=";
        trace += parameters ? "1" : "0";
        if (parameters) {
            std::array<std::uint32_t, 24> words;
            std::memcpy(words.data(), parameters, sizeof(words));
            trace += " prefix96=";
            for (const auto value : words) {
                std::array<char, 10> text{};
                (void)std::snprintf(text.data(), text.size(), "%08x ", static_cast<unsigned>(value));
                trace.append(text.data(), 9);
            }
        }
        trace += '\n';
        diagnostics::write_service_trace(trace);
        auto &runtime = *current_runtime();
        if (!runtime.graphics_devices().configured()) {
            diagnostics::stop_trace("native graphics backend is not configured; no device was returned", 78);
        }
        if (adapter) {
            errno = saved_errno;
            return static_cast<std::int32_t>(0x80070057u);
        }
        auto creation = runtime.graphics_memory().begin_device_creation();
        const auto result = runtime.graphics_devices().create(interface_id, output, creation.plan());
        if (result == 0) {
            creation.succeeded();
            diagnostics::write_service_trace("TRACE native graphics real device frontend created\n");
        }
        errno = saved_errno;
        return result;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}

std::int32_t AMBROSIA_GUEST_ABI configure_memory(const void *description) noexcept {
    if (!description) {
        return static_cast<std::int32_t>(0x80004003u); // E_POINTER.
    }
    try {
        auto *runtime = current_runtime();
        if (!runtime) {
            diagnostics::stop_trace("graphics memory policy requires a scoped runtime", 78);
        }
        // Only the observed 32-byte caller profile is supported. Copy once;
        // validation never writes guest input or retains its stack pointer.
        std::array<std::byte, 32> copy{};
        std::memcpy(copy.data(), description, copy.size());
        const auto result = runtime->graphics_memory().configure(copy);
        diagnostics::write_service_trace(result == 0 ? "TRACE graphics-memory configuration accepted\n"
                                                     : "TRACE graphics-memory configuration rejected\n");
        return result;
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    }
}
} // namespace

void register_graphics_services(ImportRegistry &registry) {
    registry.add("d3d12_xs.dll", "D3D12XboxCreateDevice", reinterpret_cast<std::uintptr_t>(&create_device));
    registry.add("d3d12_xs.dll", "D3D12ConfigureVirtualMemoryX",
                 reinterpret_cast<std::uintptr_t>(&configure_memory));
}
} // namespace ambrosia::services
