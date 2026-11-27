#include "graphics_devices.h"
#include "guest_thread.h"
#include "host_graphics.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
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

constexpr Guid pageable_interface{
    0x63ee58fb, 0x1268, 0x4835, {0x86, 0xda, 0xf0, 0x08, 0xce, 0x62, 0xf0, 0xd6}};
constexpr Guid device_child_interface{
    0x905db94b, 0xa00c, 0x4140, {0x9d, 0xf5, 0x2b, 0x64, 0xca, 0x9e, 0xa3, 0x57}};

bool supports_fence(const void *interface_id) {
    Guid requested;
    std::memcpy(&requested, interface_id, sizeof(requested));
    return requested == unknown_interface_id || requested == supported_interfaces.front() ||
           requested == device_child_interface || requested == pageable_interface ||
           requested == fence_interface;
}

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

    struct Fence {
        const std::uintptr_t *table;
        std::uint32_t references = 1;
        std::uint32_t real = 0;
        Device *parent = nullptr;
    };

    static_assert(offsetof(Device, table) == 0);
    static_assert(offsetof(Fence, table) == 0);

    std::mutex mutex;
    std::unique_ptr<host::Graphics> backend;
    std::unique_ptr<Device> device;
    std::map<Fence *, std::unique_ptr<Fence>> fences;

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

    static Access owned_fence(Fence *pointer) {
        auto *runtime = current_runtime();
        if (!runtime || !guest::ThreadContext::current()) {
            diagnostics::stop_trace("native fence requires an active guest runtime", 78);
        }
        auto &state = *runtime->graphics_devices().state_;
        std::unique_lock lock(state.mutex);
        if (!pointer || !state.fences.contains(pointer) || !state.backend || !state.device) {
            diagnostics::stop_trace("native fence is not owned by this runtime", 78);
        }
        return {state, std::move(lock)};
    }

    static void release_device(State &state, Device &pointer) {
        if (pointer.references > 1) {
            --pointer.references;
        } else {
            state.backend->destroy_device(pointer.real.id);
            state.device.reset();
        }
    }

    template <std::size_t Slot> static std::uintptr_t AMBROSIA_GUEST_ABI unsupported_fence(Fence *) noexcept {
        diagnostics::stop_trace("unsupported native ID3D12Fence slot " + std::to_string(Slot), 78);
    }

    static std::int32_t AMBROSIA_GUEST_ABI fence_query(Fence *pointer, const void *interface_id,
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
            auto access = owned_fence(pointer);
            if (!supports_fence(interface_id)) {
                return no_interface;
            }
            increment_fence(*pointer);
            *output = pointer;
            return 0;
        } catch (const std::exception &error) {
            diagnostics::stop_trace(error.what(), 78);
        }
    }

    static void increment_fence(Fence &pointer) {
        if (pointer.references == UINT32_MAX) {
            diagnostics::stop_trace("native fence reference count exhausted", 78);
        }
        ++pointer.references;
    }

    static std::uint32_t AMBROSIA_GUEST_ABI fence_add(Fence *pointer) noexcept {
        const PreserveErrno preserve;
        try {
            auto access = owned_fence(pointer);
            increment_fence(*pointer);
            return pointer->references;
        } catch (const std::exception &error) {
            diagnostics::stop_trace(error.what(), 78);
        }
    }

    static std::uint32_t AMBROSIA_GUEST_ABI fence_release(Fence *pointer) noexcept {
        const PreserveErrno preserve;
        try {
            auto access = owned_fence(pointer);
            const auto remaining = pointer->references - 1;
            if (remaining) {
                pointer->references = remaining;
            } else {
                access.state.backend->destroy_fence(pointer->real);
                auto *parent = pointer->parent;
                access.state.fences.erase(pointer);
                release_device(access.state, *parent);
            }
            return remaining;
        } catch (const std::exception &error) {
            diagnostics::stop_trace(error.what(), 78);
        }
    }

    static std::int32_t AMBROSIA_GUEST_ABI fence_device(Fence *pointer, const void *interface_id,
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
            auto access = owned_fence(pointer);
            if (!supports(interface_id)) {
                return no_interface;
            }
            increment(*pointer->parent);
            *output = pointer->parent;
            return 0;
        } catch (const std::exception &error) {
            diagnostics::stop_trace(error.what(), 78);
        }
    }

    static std::uint64_t AMBROSIA_GUEST_ABI fence_completed(Fence *pointer) noexcept {
        const PreserveErrno preserve;
        try {
            auto access = owned_fence(pointer);
            return access.state.backend->completed_fence(pointer->real);
        } catch (const std::exception &error) {
            diagnostics::stop_trace(error.what(), 78);
        }
    }

    static std::int32_t AMBROSIA_GUEST_ABI fence_signal(Fence *pointer, std::uint64_t value) noexcept {
        const PreserveErrno preserve;
        try {
            auto access = owned_fence(pointer);
            if (value == UINT64_MAX) {
                return static_cast<std::int32_t>(0x80070057u);
            }
            const auto completed = access.state.backend->completed_fence(pointer->real);
            if (value < completed) {
                diagnostics::stop_trace("native timeline cannot rewind a D3D12 fence", 78);
            }
            access.state.backend->signal_fence(pointer->real, value);
            return 0;
        } catch (const std::exception &error) {
            diagnostics::stop_trace(error.what(), 78);
        }
    }

    template <std::size_t... Slots>
    static std::array<std::uintptr_t, sizeof...(Slots)> make_fence_table(std::index_sequence<Slots...>) {
        return {reinterpret_cast<std::uintptr_t>(&unsupported_fence<Slots>)...};
    }

    static const auto &fence_table() {
        static const auto entries = [] {
            auto result = make_fence_table(std::make_index_sequence<11>{});
            result[0] = reinterpret_cast<std::uintptr_t>(&fence_query);
            result[1] = reinterpret_cast<std::uintptr_t>(&fence_add);
            result[2] = reinterpret_cast<std::uintptr_t>(&fence_release);
            result[7] = reinterpret_cast<std::uintptr_t>(&fence_device);
            result[8] = reinterpret_cast<std::uintptr_t>(&fence_completed);
            result[10] = reinterpret_cast<std::uintptr_t>(&fence_signal);
            return result;
        }();
        return entries;
    }

    static std::int32_t AMBROSIA_GUEST_ABI create_fence(Device *pointer, std::uint64_t initial,
                                                        std::uint32_t flags, const void *interface_id,
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
            if (!supports_fence(interface_id)) {
                return no_interface;
            }
            if (flags || initial == UINT64_MAX) {
                diagnostics::stop_trace("unsupported native fence creation flags or initial value", 78);
            }
            if (pointer->references == UINT32_MAX || access.state.fences.size() >= 32) {
                diagnostics::stop_trace("native fence ownership capacity exhausted", 78);
            }
            auto fence = std::make_unique<Fence>();
            fence->table = fence_table().data();
            fence->parent = pointer;
            auto *result = fence.get();
            // Allocate map ownership before asking the host for a real semaphore.
            auto [position, inserted] = access.state.fences.emplace(result, std::move(fence));
            if (!inserted) {
                throw std::runtime_error("native fence object ownership collision");
            }
            try {
                result->real = access.state.backend->create_fence(initial);
            } catch (...) {
                access.state.fences.erase(position);
                throw;
            }
            ++pointer->references; // The child retains its real parent device.
            *output = result;
            diagnostics::write_service_trace("TRACE native real timeline fence created\n");
            return 0;
        } catch (const std::exception &error) {
            diagnostics::stop_trace(error.what(), 78);
        }
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
            release_device(access.state, *pointer);
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

    struct HeapProperties {
        std::uint32_t type = 4;
        std::uint32_t cpu_page = 0;
        std::uint32_t memory_pool = 0;
        std::uint32_t creation_node = 1;
        std::uint32_t visible_node = 1;
    };

    static_assert(sizeof(HeapProperties) == 20);

    // SDK C-style calls name This before the result buffer. An ms_abi
    // function-pointer returning the aggregate uses the compiler's hidden
    // result before This. Recognize the receiver by exact owned identity,
    // never by dereferencing or guessing either incoming pointer.
    static HeapProperties *AMBROSIA_GUEST_ABI heap_properties(void *first, void *second,
                                                              std::uint32_t node_mask,
                                                              std::uint32_t heap_type) noexcept {
        const PreserveErrno preserve;
        try {
            auto *runtime = current_runtime();
            if (!runtime || !guest::ThreadContext::current()) {
                diagnostics::stop_trace("native heap properties require an active guest runtime", 78);
            }
            auto &state = *runtime->graphics_devices().state_;
            std::unique_lock lock(state.mutex);
            const bool first_receiver = state.device && first == state.device.get();
            const bool second_receiver = state.device && second == state.device.get();
            if (first_receiver == second_receiver || !state.backend) {
                diagnostics::stop_trace("native heap properties receiver layout is not owned", 78);
            }
            auto *output = static_cast<HeapProperties *>(first_receiver ? second : first);
            char message[128];
            const auto count = std::snprintf(
                message, sizeof(message),
                "TRACE native heap properties output_present=%u return_first=%u node=%u type=%u\n",
                output ? 1u : 0u, second_receiver ? 1u : 0u, node_mask, heap_type);
            if (count > 0 && static_cast<std::size_t>(count) < sizeof(message)) {
                diagnostics::write_service_trace(std::string_view(message, static_cast<std::size_t>(count)));
            }
            if (!output || node_mask > 1 || heap_type < 1 || heap_type > 3) {
                diagnostics::stop_trace("unsupported native custom heap properties request", 78);
            }
            const bool unified = state.backend->unified_memory();
            HeapProperties result;
            if (heap_type == 1) {
                result.cpu_page = 1; // NOT_AVAILABLE.
                result.memory_pool = unified ? 1 : 2;
            } else {
                result.cpu_page = heap_type == 2 ? 2 : 3; // WRITE_COMBINE / WRITE_BACK.
                result.memory_pool = 1;
            }
            std::memcpy(output, &result, sizeof(result));
            return output;
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
            result[26] = reinterpret_cast<std::uintptr_t>(&heap_properties);
            result[36] = reinterpret_cast<std::uintptr_t>(&create_fence);
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
