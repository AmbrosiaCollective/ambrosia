#include "graphics_devices.h"
#include "guest_thread.h"
#include "host_graphics.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace ambrosia::services {
namespace {

struct PreserveErrno {
    int saved = errno;

    ~PreserveErrno() {
        errno = saved;
    }
};

// Public interface identities, not captured game bytes. The pinned SDK's
// ID3D12Device11 inherits these interfaces and has 80 vtable slots.
constexpr std::array<Guid, 13> supported_interfaces{{
    {0xc4fec28f, 0x7966, 0x4e95, {0x9f, 0x94, 0xf4, 0x31, 0xcb, 0x56, 0xc3, 0xb8}}, // ID3D12Object
    {0x189819f1, 0x1db6, 0x4b57, {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}}, // ID3D12Device
    {0x77acce80, 0x638e, 0x4e65, {0x88, 0x95, 0xc1, 0xf2, 0x33, 0x86, 0x86, 0x3e}}, // ID3D12Device1
    {0x30baa41e, 0xb15b, 0x475c, {0xa0, 0xbb, 0x1a, 0xf5, 0xc5, 0xb6, 0x43, 0x28}}, // ID3D12Device2
    {0x81dadc15, 0x2bad, 0x4392, {0x93, 0xc5, 0x10, 0x13, 0x45, 0xc4, 0xaa, 0x98}}, // ID3D12Device3
    {0xe865df17, 0xa9ee, 0x46f9, {0xa4, 0x63, 0x30, 0x98, 0x31, 0x5a, 0xa2, 0xe5}}, // ID3D12Device4
    {0x8b4f173b, 0x2fea, 0x4b80, {0x8f, 0x58, 0x43, 0x07, 0x19, 0x1a, 0xb9, 0x5d}}, // ID3D12Device5
    {0xc70b221b, 0x40e4, 0x4a17, {0x89, 0xaf, 0x02, 0x5a, 0x07, 0x27, 0xa6, 0xdc}}, // ID3D12Device6
    {0x5c014b53, 0x68a1, 0x4b9b, {0x8b, 0xd1, 0xdd, 0x60, 0x46, 0xb9, 0x35, 0x8b}}, // ID3D12Device7
    {0x9218e6bb, 0xf944, 0x4f7e, {0xa7, 0x5c, 0xb1, 0xb2, 0xc7, 0xb7, 0x01, 0xf3}}, // ID3D12Device8
    {0x4c80e962, 0xf032, 0x4f60, {0xbc, 0x9e, 0xeb, 0xc2, 0xcf, 0xa1, 0xd8, 0x3c}}, // ID3D12Device9
    {0x517f8718, 0xaa66, 0x49f9, {0xb0, 0x2b, 0xa7, 0xab, 0x89, 0xc0, 0x60, 0x31}}, // ID3D12Device10
    {0x5405c344, 0xd457, 0x444e, {0xb4, 0xdd, 0x23, 0x66, 0xe4, 0x5a, 0xee, 0x39}}, // ID3D12Device11
}};

bool supports(const void *interface_id) {
    Guid requested;
    std::memcpy(&requested, interface_id, sizeof(requested));
    return requested == unknown_interface_id ||
           std::find(supported_interfaces.begin(), supported_interfaces.end(), requested) !=
               supported_interfaces.end();
}
} // namespace

struct GraphicsDevices::State {
    struct Device {
        const std::uintptr_t *table;
        std::uint32_t references = 1;
        host::GraphicsDevice real;
        // Coverage is a future resource-mapping constraint, not a claim that
        // logical-device creation reserves a contiguous host GPU address range.
        std::optional<GraphicsMemoryPlan> memory_plan;
    };

    static_assert(offsetof(Device, table) == 0);

    std::mutex mutex;
    std::unique_ptr<host::Graphics> backend;
    std::unique_ptr<Device> device;

    struct Access {
        State &state;
        std::unique_lock<std::mutex> lock;
    };

    static Access owned(Device *pointer) {
        auto *runtime = current_runtime();
        if (!runtime || !guest::ThreadContext::current()) {
            diagnostics::stop_trace("native graphics device requires an active guest runtime", 78);
        }
        auto &state = *runtime->graphics_devices().state_;
        std::unique_lock lock(state.mutex);
        if (!pointer || state.device.get() != pointer || !state.backend) {
            diagnostics::stop_trace("native graphics device is not owned by this runtime", 78);
        }
        return {state, std::move(lock)};
    }

    template <std::size_t Slot> static std::uintptr_t AMBROSIA_GUEST_ABI unsupported(Device *) noexcept {
        diagnostics::stop_trace("unsupported native ID3D12Device11 slot " + std::to_string(Slot), 78);
    }

    static std::int32_t AMBROSIA_GUEST_ABI query_interface(Device *pointer, const void *interface_id,
                                                           void **output) noexcept {
        const PreserveErrno preserve;
        if (!output) {
            return pointer_error;
        }
        *output = nullptr;
        if (!interface_id) {
            return pointer_error;
        }
        try {
            auto access = owned(pointer);
            if (!supports(interface_id)) {
                return no_interface;
            }
            increment(*pointer);
            *output = pointer;
            return 0;
        } catch (const std::exception &error) {
            diagnostics::stop_trace(error.what(), 78);
        }
    }

    static void increment(Device &device) {
        if (device.references == std::numeric_limits<std::uint32_t>::max()) {
            diagnostics::stop_trace("native graphics reference count exhausted", 78);
        }
        ++device.references;
    }

    static std::uint32_t AMBROSIA_GUEST_ABI add_reference(Device *pointer) noexcept {
        const PreserveErrno preserve;
        try {
            auto access = owned(pointer);
            increment(*pointer);
            return pointer->references;
        } catch (const std::exception &error) {
            diagnostics::stop_trace(error.what(), 78);
        }
    }

    static std::uint32_t AMBROSIA_GUEST_ABI release(Device *pointer) noexcept {
        const PreserveErrno preserve;
        try {
            auto access = owned(pointer);
            const auto remaining = pointer->references - 1;
            if (remaining) {
                pointer->references = remaining;
            } else {
                access.state.backend->destroy_device(pointer->real.id);
                access.state.device.reset();
            }
            return remaining;
        } catch (const std::exception &error) {
            diagnostics::stop_trace(error.what(), 78);
        }
    }

    static std::uint32_t AMBROSIA_GUEST_ABI node_count(Device *pointer) noexcept {
        const PreserveErrno preserve;
        try {
            auto access = owned(pointer);
            (void)access.state.backend->query_device(pointer->real.id);
            // The host currently selects exactly one physical device and never
            // creates a linked-adapter group. Verify its live owner before use.
            return 1;
        } catch (const std::exception &error) {
            diagnostics::stop_trace(error.what(), 78);
        }
    }

    template <std::size_t... Slots>
    static std::array<std::uintptr_t, sizeof...(Slots)> make_table(std::index_sequence<Slots...>) {
        return {reinterpret_cast<std::uintptr_t>(&unsupported<Slots>)...};
    }

    static const auto &table() {
        static const auto entries = [] {
            auto result = make_table(std::make_index_sequence<80>{});
            result[0] = reinterpret_cast<std::uintptr_t>(&query_interface);
            result[1] = reinterpret_cast<std::uintptr_t>(&add_reference);
            result[2] = reinterpret_cast<std::uintptr_t>(&release);
            result[7] = reinterpret_cast<std::uintptr_t>(&node_count);
            return result;
        }();
        return entries;
    }
};

GraphicsDevices::GraphicsDevices() : state_(std::make_unique<State>()) {}

GraphicsDevices::~GraphicsDevices() = default;

void GraphicsDevices::bind(const std::filesystem::path &endpoint_root) {
    const std::lock_guard lock(state_->mutex);
    if (state_->backend) {
        throw std::runtime_error("native graphics endpoints are already configured");
    }
    state_->backend = std::make_unique<host::Graphics>(endpoint_root);
}

bool GraphicsDevices::configured() const {
    const std::lock_guard lock(state_->mutex);
    return bool(state_->backend);
}

std::int32_t GraphicsDevices::create(const void *interface_id, void **output,
                                     std::optional<GraphicsMemoryPlan> memory_plan) {
    if (!output) {
        return pointer_error;
    }
    *output = nullptr;
    if (!interface_id) {
        return pointer_error;
    }
    const std::lock_guard lock(state_->mutex);
    if (!state_->backend || state_->device) {
        throw std::runtime_error("native graphics requires a configured session without a live device");
    }
    if (!supports(interface_id)) {
        return no_interface;
    }
    auto device = std::make_unique<State::Device>();
    device->table = State::table().data();
    device->memory_plan = memory_plan;
    device->real = state_->backend->create_device();
    state_->device = std::move(device);
    *output = state_->device.get();
    return 0;
}
} // namespace ambrosia::services
