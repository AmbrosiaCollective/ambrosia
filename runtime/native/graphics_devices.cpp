#include "graphics_devices.h"
#include "graphics_recording.h"
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

bool supports_queue(const void *interface_id) {
    Guid requested;
    std::memcpy(&requested, interface_id, sizeof(requested));
    return requested == unknown_interface_id || requested == supported_interfaces.front() ||
           requested == device_child_interface || requested == pageable_interface ||
           requested == command_queue_interface;
}

bool supports_list(const void *interface_id) {
    Guid requested;
    std::memcpy(&requested, interface_id, sizeof(requested));
    return requested == unknown_interface_id || requested == supported_interfaces.front() ||
           requested == device_child_interface || requested == command_list_interface ||
           requested == graphics_command_list_interface || requested == console_command_list_interface;
}

bool supports_allocator(const void *interface_id) {
    Guid requested;
    std::memcpy(&requested, interface_id, sizeof(requested));
    return requested == unknown_interface_id || requested == supported_interfaces.front() ||
           requested == device_child_interface || requested == pageable_interface ||
           requested == command_allocator_interface;
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
        std::uint32_t creation_flags = 0;
        Device *parent = nullptr;
    };

    struct QueueDescription {
        std::uint32_t type = 0;
        std::int32_t priority = 0;
        // DISABLE_GPU_TIMEOUT is retained as queue policy metadata. It does
        // not remove sandbox deadlines or change the host driver's reset policy.
        std::uint32_t flags = 0, node_mask = 0;
    };

    static_assert(sizeof(QueueDescription) == 16);
    static_assert(offsetof(QueueDescription, priority) == 4);
    static_assert(offsetof(QueueDescription, flags) == 8);
    static_assert(offsetof(QueueDescription, node_mask) == 12);

    // The public FramePacing sample names these console fields. Their
    // four-word layout is the bounded HLE interpretation of the measured call,
    // not the PC descriptor's Priority/Flags/NodeMask layout.
    struct XboxQueueDescription {
        std::uint32_t type = 0;
        std::uint32_t flags = 0;
        std::uint32_t engine_or_pipe_index = 0;
        std::uint32_t queue_index = 0;
    };

    static_assert(sizeof(XboxQueueDescription) == 16);

    struct Queue {
        const std::uintptr_t *table;
        std::uint32_t references = 1;
        Device *parent = nullptr;
        QueueDescription description;
        std::optional<XboxQueueDescription> xbox_selection;
    };

    static_assert(offsetof(Queue, table) == 0);

    static_assert(offsetof(Device, table) == 0);
    static_assert(offsetof(Fence, table) == 0);

    struct Allocator {
        const std::uintptr_t *table;
        std::uint32_t references = 1;
        std::uint32_t type = 0;
        Device *parent = nullptr;
        // Lists own the portable command IR. One list may record into this
        // allocator at a time, matching the existing renderer's CPU model.
        const void *recording = nullptr;
    };

    static_assert(offsetof(Allocator, table) == 0);

    struct List {
        std::uint32_t references = 1;
        std::uint32_t type = 0;
        Device *parent = nullptr;
        Allocator *active_allocator = nullptr;
        // Preserve uninterpreted creation hints; do not reinterpret them as PC flags.
        std::array<std::uint32_t, 4> description{};
        GraphicsRecording recording;

        explicit List(const std::uintptr_t *table) : recording(table) {}
    };

    std::mutex mutex;
    std::unique_ptr<host::Graphics> backend;
    std::unique_ptr<Device> device;
    std::map<Fence *, std::unique_ptr<Fence>> fences;
    std::map<Queue *, std::unique_ptr<Queue>> queues;
    std::map<Allocator *, std::unique_ptr<Allocator>> allocators;
    std::map<void *, std::unique_ptr<List>> lists;

    struct Access {
        State &state;
        std::unique_lock<std::mutex> lock;
    };

    struct ListAccess {
        State &state;
        std::unique_lock<std::mutex> lock;
        List &list;
    };

    static ListAccess owned_list(void *pointer) {
        auto *runtime = current_runtime();
        if (!runtime || !guest::ThreadContext::current()) {
            diagnostics::stop_trace("native command list requires an active guest runtime", 78);
        }
        auto &state = *runtime->graphics_devices().state_;
        std::unique_lock lock(state.mutex);
        const auto found = state.lists.find(pointer);
        if (found == state.lists.end() || !state.backend || !state.device ||
            found->second->parent != state.device.get()) {
            diagnostics::stop_trace("native command list is not owned by this runtime", 78);
        }
        return {state, std::move(lock), *found->second};
    }

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

    static Access owned_queue(Queue *pointer) {
        auto *runtime = current_runtime();
        if (!runtime || !guest::ThreadContext::current()) {
            diagnostics::stop_trace("native command queue requires an active guest runtime", 78);
        }
        auto &state = *runtime->graphics_devices().state_;
        std::unique_lock lock(state.mutex);
        if (!pointer || !state.queues.contains(pointer) || !state.backend || !state.device) {
            diagnostics::stop_trace("native command queue is not owned by this runtime", 78);
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

    static Access owned_allocator(Allocator *pointer) {
        auto *runtime = current_runtime();
        if (!runtime || !guest::ThreadContext::current()) {
            diagnostics::stop_trace("native command allocator requires an active guest runtime", 78);
        }
        auto &state = *runtime->graphics_devices().state_;
        std::unique_lock lock(state.mutex);
        if (!pointer || !state.allocators.contains(pointer) || !state.backend || !state.device) {
            diagnostics::stop_trace("native command allocator is not owned by this runtime", 78);
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

    template <std::size_t Slot>
    static std::uintptr_t AMBROSIA_GUEST_ABI unsupported_allocator(Allocator *) noexcept {
        diagnostics::stop_trace("unsupported native ID3D12CommandAllocator slot " + std::to_string(Slot), 78);
    }

    static void increment_allocator(Allocator &pointer) {
        if (pointer.references == UINT32_MAX) {
            diagnostics::stop_trace("native command allocator reference count exhausted", 78);
        }
        ++pointer.references;
    }

    static std::int32_t AMBROSIA_GUEST_ABI allocator_query(Allocator *pointer, const void *interface_id,
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
            auto access = owned_allocator(pointer);
            if (!supports_allocator(interface_id)) {
                return no_interface;
            }
            increment_allocator(*pointer);
            *output = pointer;
            return 0;
        } catch (const std::exception &error) {
            diagnostics::stop_trace(error.what(), 78);
        }
    }

    static std::uint32_t AMBROSIA_GUEST_ABI allocator_add(Allocator *pointer) noexcept {
        const PreserveErrno preserve;
        try {
            auto access = owned_allocator(pointer);
            increment_allocator(*pointer);
            return pointer->references;
        } catch (const std::exception &error) {
            diagnostics::stop_trace(error.what(), 78);
        }
    }

    // Internal callers already hold the State lock, including a list dropping
    // its recording lease. Keep the guest entrypoint from recursively locking.
    static std::uint32_t release_allocator(State &state, Allocator &pointer) {
        const auto remaining = pointer.references - 1;
        if (remaining) {
            pointer.references = remaining;
        } else {
            if (pointer.recording) {
                diagnostics::stop_trace("native command allocator lost its recording lease", 78);
            }
            auto *parent = pointer.parent;
            state.allocators.erase(&pointer);
            release_device(state, *parent);
        }
        return remaining;
    }

    static std::uint32_t AMBROSIA_GUEST_ABI allocator_release(Allocator *pointer) noexcept {
        const PreserveErrno preserve;
        try {
            auto access = owned_allocator(pointer);
            return release_allocator(access.state, *pointer);
        } catch (const std::exception &error) {
            diagnostics::stop_trace(error.what(), 78);
        }
    }

    static std::int32_t AMBROSIA_GUEST_ABI allocator_device(Allocator *pointer, const void *interface_id,
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
            auto access = owned_allocator(pointer);
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

    static std::int32_t AMBROSIA_GUEST_ABI allocator_reset(Allocator *pointer) noexcept {
        const PreserveErrno preserve;
        try {
            auto access = owned_allocator(pointer);
            return pointer->recording ? static_cast<std::int32_t>(0x80004005u) : 0;
        } catch (const std::exception &error) {
            diagnostics::stop_trace(error.what(), 78);
        }
    }

    template <std::size_t... Slots>
    static std::array<std::uintptr_t, sizeof...(Slots)> make_allocator_table(std::index_sequence<Slots...>) {
        return {reinterpret_cast<std::uintptr_t>(&unsupported_allocator<Slots>)...};
    }

    static const auto &allocator_table() {
        static const auto entries = [] {
            auto result = make_allocator_table(std::make_index_sequence<64>{});
            result[0] = reinterpret_cast<std::uintptr_t>(&allocator_query);
            result[1] = reinterpret_cast<std::uintptr_t>(&allocator_add);
            result[2] = reinterpret_cast<std::uintptr_t>(&allocator_release);
            result[7] = reinterpret_cast<std::uintptr_t>(&allocator_device);
            result[8] = reinterpret_cast<std::uintptr_t>(&allocator_reset);
            return result;
        }();
        return entries;
    }

    static std::int32_t AMBROSIA_GUEST_ABI create_allocator(Device *pointer, std::uint32_t type,
                                                            const void *interface_id,
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
            if (!supports_allocator(interface_id)) {
                return no_interface;
            }
            if (type > 6) {
                return static_cast<std::int32_t>(0x80070057u);
            }
            if (type > 3) {
                return static_cast<std::int32_t>(0x80004001u);
            }
            if (access.state.allocators.size() >= 64 || pointer->references == UINT32_MAX) {
                diagnostics::stop_trace("native command allocator ownership capacity exhausted", 78);
            }
            (void)access.state.backend->query_device(pointer->real.id);
            auto allocator = std::make_unique<Allocator>();
            allocator->table = allocator_table().data();
            allocator->type = type;
            allocator->parent = pointer;
            auto *result = allocator.get();
            auto [position, inserted] = access.state.allocators.emplace(result, std::move(allocator));
            (void)position;
            if (!inserted) {
                throw std::runtime_error("native command allocator ownership collision");
            }
            ++pointer->references;
            *output = result;
            char message[96];
            const auto count = std::snprintf(message, sizeof(message),
                                             "TRACE native CPU command allocator created type=%u\n", type);
            if (count > 0 && static_cast<std::size_t>(count) < sizeof(message)) {
                diagnostics::write_service_trace(std::string_view(message, static_cast<std::size_t>(count)));
            }
            return 0;
        } catch (const std::exception &error) {
            diagnostics::stop_trace(error.what(), 78);
        }
    }

    template <std::size_t Slot> static std::uintptr_t AMBROSIA_GUEST_ABI unsupported_queue(Queue *) noexcept {
        diagnostics::stop_trace("unsupported native ID3D12CommandQueue slot " + std::to_string(Slot), 78);
    }

    static void increment_queue(Queue &pointer) {
        if (pointer.references == UINT32_MAX) {
            diagnostics::stop_trace("native command queue reference count exhausted", 78);
        }
        ++pointer.references;
    }

    static std::int32_t AMBROSIA_GUEST_ABI queue_query(Queue *pointer, const void *interface_id,
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
            auto access = owned_queue(pointer);
            if (!supports_queue(interface_id)) {
                return no_interface;
            }
            increment_queue(*pointer);
            *output = pointer;
            return 0;
        } catch (const std::exception &error) {
            diagnostics::stop_trace(error.what(), 78);
        }
    }

    static std::uint32_t AMBROSIA_GUEST_ABI queue_add(Queue *pointer) noexcept {
        const PreserveErrno preserve;
        try {
            auto access = owned_queue(pointer);
            increment_queue(*pointer);
            return pointer->references;
        } catch (const std::exception &error) {
            diagnostics::stop_trace(error.what(), 78);
        }
    }

    static std::uint32_t AMBROSIA_GUEST_ABI queue_release(Queue *pointer) noexcept {
        const PreserveErrno preserve;
        try {
            auto access = owned_queue(pointer);
            const auto remaining = pointer->references - 1;
            if (remaining) {
                pointer->references = remaining;
            } else {
                auto *parent = pointer->parent;
                access.state.queues.erase(pointer);
                // The host queue belongs to the device. The last parent lease
                // releases the real backend, not merely a frontend descriptor.
                release_device(access.state, *parent);
            }
            return remaining;
        } catch (const std::exception &error) {
            diagnostics::stop_trace(error.what(), 78);
        }
    }

    static std::int32_t AMBROSIA_GUEST_ABI queue_device(Queue *pointer, const void *interface_id,
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
            auto access = owned_queue(pointer);
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

    // A compiled ms_abi aggregate return passes its hidden result first;
    // the SDK's explicit C-style form passes This first. Resolve only exact
    // owned identities before writing the caller's 16-byte result.
    static QueueDescription *AMBROSIA_GUEST_ABI queue_description(void *first, void *second) noexcept {
        const PreserveErrno preserve;
        try {
            auto *runtime = current_runtime();
            if (!runtime || !guest::ThreadContext::current()) {
                diagnostics::stop_trace("native queue description requires an active guest runtime", 78);
            }
            auto &state = *runtime->graphics_devices().state_;
            std::unique_lock lock(state.mutex);
            const bool first_receiver = state.queues.contains(static_cast<Queue *>(first));
            const bool second_receiver = state.queues.contains(static_cast<Queue *>(second));
            if (first_receiver == second_receiver || !state.backend || !state.device) {
                diagnostics::stop_trace("native queue description receiver layout is not owned", 78);
            }
            auto *pointer = static_cast<Queue *>(first_receiver ? first : second);
            auto *output = static_cast<QueueDescription *>(first_receiver ? second : first);
            if (!output) {
                diagnostics::stop_trace("native queue description requires an output buffer", 78);
            }
            std::memcpy(output, &pointer->description, sizeof(*output));
            return output;
        } catch (const std::exception &error) {
            diagnostics::stop_trace(error.what(), 78);
        }
    }

    template <std::size_t... Slots>
    static std::array<std::uintptr_t, sizeof...(Slots)> make_queue_table(std::index_sequence<Slots...>) {
        return {reinterpret_cast<std::uintptr_t>(&unsupported_queue<Slots>)...};
    }

    static const auto &queue_table() {
        static const auto entries = [] {
            auto result = make_queue_table(std::make_index_sequence<128>{});
            result[0] = reinterpret_cast<std::uintptr_t>(&queue_query);
            result[1] = reinterpret_cast<std::uintptr_t>(&queue_add);
            result[2] = reinterpret_cast<std::uintptr_t>(&queue_release);
            result[7] = reinterpret_cast<std::uintptr_t>(&queue_device);
            result[18] = reinterpret_cast<std::uintptr_t>(&queue_description);
            return result;
        }();
        return entries;
    }

    // Both entrypoints hold State::mutex and validate caller pointers before
    // transferring an immutable description and a parent lease to this owner.
    static std::int32_t create_owned_queue(State &state, Device *pointer, const QueueDescription &copy,
                                           std::optional<XboxQueueDescription> xbox_selection,
                                           void **output) {
        if ((copy.type != 0 && copy.type != 2 && copy.type != 3) || copy.priority || (copy.flags & ~1u) ||
            copy.node_mask > 1 || state.queues.size() >= 8 || pointer->references == UINT32_MAX) {
            diagnostics::stop_trace("unsupported native command queue descriptor or ownership capacity", 78);
        }
        // Logical queues lease the real graphics/compute queue. Independent
        // hardware-engine scheduling and GPU submission are not implemented.
        (void)state.backend->query_device(pointer->real.id);
        auto queue = std::make_unique<Queue>();
        queue->table = queue_table().data();
        queue->parent = pointer;
        queue->description = copy;
        queue->xbox_selection = xbox_selection;
        auto *result = queue.get();
        auto [position, inserted] = state.queues.emplace(result, std::move(queue));
        (void)position;
        if (!inserted) {
            throw std::runtime_error("native queue ownership collision");
        }
        ++pointer->references;
        *output = result;
        diagnostics::write_service_trace("TRACE native real command queue frontend created\n");
        return 0;
    }

    static std::int32_t AMBROSIA_GUEST_ABI create_queue(Device *pointer, const QueueDescription *description,
                                                        const void *interface_id, void **output) noexcept {
        const PreserveErrno preserve;
        if (!output) {
            return pointer_error;
        }
        *output = nullptr;
        if (!interface_id || !description) {
            return pointer_error;
        }
        try {
            auto access = owned(pointer);
            if (!supports_queue(interface_id)) {
                return no_interface;
            }
            QueueDescription copy;
            std::memcpy(&copy, description, sizeof(copy));
            char message[160];
            const auto count = std::snprintf(
                message, sizeof(message), "TRACE native queue request type=%u priority=%d flags=%u node=%u\n",
                copy.type, copy.priority, copy.flags, copy.node_mask);
            if (count > 0 && static_cast<std::size_t>(count) < sizeof(message)) {
                diagnostics::write_service_trace(std::string_view(message, static_cast<std::size_t>(count)));
            }
            return create_owned_queue(access.state, pointer, copy, std::nullopt, output);
        } catch (const std::exception &error) {
            diagnostics::stop_trace(error.what(), 78);
        }
    }

    static std::int32_t AMBROSIA_GUEST_ABI create_queue_x(Device *pointer,
                                                          const XboxQueueDescription *description,
                                                          const void *interface_id, void **output) noexcept {
        const PreserveErrno preserve;
        if (!output) {
            return pointer_error;
        }
        *output = nullptr;
        if (!interface_id || !description) {
            return pointer_error;
        }
        try {
            auto access = owned(pointer);
            if (!supports_queue(interface_id)) {
                return no_interface;
            }
            XboxQueueDescription selection;
            std::memcpy(&selection, description, sizeof(selection));
            char message[160];
            const auto count = std::snprintf(
                message, sizeof(message),
                "TRACE native Xbox queue request type=%u flags=%x engine=%u queue=%u\n", selection.type,
                selection.flags, selection.engine_or_pipe_index, selection.queue_index);
            if (count > 0 && static_cast<std::size_t>(count) < sizeof(message)) {
                diagnostics::write_service_trace(std::string_view(message, static_cast<std::size_t>(count)));
            }
            if (selection.flags || selection.engine_or_pipe_index > 3 || selection.queue_index > 1) {
                diagnostics::stop_trace("unsupported native Xbox command queue selection", 78);
            }
            QueueDescription normalized;
            normalized.type = selection.type;
            normalized.node_mask = 1;
            return create_owned_queue(access.state, pointer, normalized, selection, output);
        } catch (const std::exception &error) {
            diagnostics::stop_trace(error.what(), 78);
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
            auto result = make_fence_table(std::make_index_sequence<64>{});
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
            char request[128];
            const auto count =
                std::snprintf(request, sizeof(request), "TRACE native fence request initial=%llu flags=%x\n",
                              static_cast<unsigned long long>(initial), flags);
            if (count > 0 && static_cast<std::size_t>(count) < sizeof(request)) {
                diagnostics::write_service_trace(std::string_view(request, static_cast<std::size_t>(count)));
            }
            if ((flags & ~1u) || initial == UINT64_MAX) {
                diagnostics::stop_trace("unsupported native fence creation flags or initial value", 78);
            }
            if (pointer->references == UINT32_MAX || access.state.fences.size() >= 32) {
                diagnostics::stop_trace("native fence ownership capacity exhausted", 78);
            }
            auto fence = std::make_unique<Fence>();
            fence->table = fence_table().data();
            fence->parent = pointer;
            fence->creation_flags = flags;
            auto *result = fence.get();
            // Allocate map ownership before asking the host for a real semaphore.
            auto [position, inserted] = access.state.fences.emplace(result, std::move(fence));
            if (!inserted) {
                throw std::runtime_error("native fence object ownership collision");
            }
            try {
                result->real = access.state.backend->create_fence(initial, flags == 1);
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

    static void increment_list(List &list) {
        if (list.references == UINT32_MAX) {
            diagnostics::stop_trace("native command-list reference count exhausted", 78);
        }
        ++list.references;
    }

    static void stop_list_recording(State &state, List &list) {
        auto *allocator = list.active_allocator;
        if (!allocator) {
            return;
        }
        if (allocator->recording != list.recording.guest_object()) {
            diagnostics::stop_trace("native command list lost its allocator recording owner", 78);
        }
        allocator->recording = nullptr;
        list.active_allocator = nullptr;
        release_allocator(state, *allocator);
    }

    template <std::size_t Slot> static std::uintptr_t AMBROSIA_GUEST_ABI unsupported_list(void *) noexcept {
        diagnostics::stop_trace("unsupported native console command-list slot " + std::to_string(Slot), 78);
    }

    static std::int32_t AMBROSIA_GUEST_ABI list_query(void *pointer, const void *interface_id,
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
            auto access = owned_list(pointer);
            if (!supports_list(interface_id)) {
                return no_interface;
            }
            increment_list(access.list);
            *output = pointer;
            return 0;
        } catch (const std::exception &error) {
            diagnostics::stop_trace(error.what(), 78);
        }
    }

    static std::uint32_t AMBROSIA_GUEST_ABI list_add(void *pointer) noexcept {
        const PreserveErrno preserve;
        try {
            auto access = owned_list(pointer);
            increment_list(access.list);
            return access.list.references;
        } catch (const std::exception &error) {
            diagnostics::stop_trace(error.what(), 78);
        }
    }

    static std::uint32_t AMBROSIA_GUEST_ABI list_release(void *pointer) noexcept {
        const PreserveErrno preserve;
        try {
            auto access = owned_list(pointer);
            const auto remaining = access.list.references - 1;
            if (remaining) {
                access.list.references = remaining;
            } else {
                stop_list_recording(access.state, access.list);
                auto *parent = access.list.parent;
                access.state.lists.erase(pointer);
                release_device(access.state, *parent);
            }
            return remaining;
        } catch (const std::exception &error) {
            diagnostics::stop_trace(error.what(), 78);
        }
    }

    static std::int32_t AMBROSIA_GUEST_ABI list_device(void *pointer, const void *interface_id,
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
            auto access = owned_list(pointer);
            if (!supports(interface_id)) {
                return no_interface;
            }
            increment(*access.list.parent);
            *output = access.list.parent;
            return 0;
        } catch (const std::exception &error) {
            diagnostics::stop_trace(error.what(), 78);
        }
    }

    static std::uint32_t AMBROSIA_GUEST_ABI list_type(void *pointer) noexcept {
        const PreserveErrno preserve;
        try {
            auto access = owned_list(pointer);
            return access.list.type;
        } catch (const std::exception &error) {
            diagnostics::stop_trace(error.what(), 78);
        }
    }

    static std::int32_t AMBROSIA_GUEST_ABI list_close(void *pointer) noexcept {
        const PreserveErrno preserve;
        try {
            auto access = owned_list(pointer);
            if (!access.list.recording.close()) {
                return static_cast<std::int32_t>(0x80004005u);
            }
            stop_list_recording(access.state, access.list);
            diagnostics::write_service_trace("TRACE native console CPU command list closed\n");
            return 0;
        } catch (const std::exception &error) {
            diagnostics::stop_trace(error.what(), 78);
        }
    }

    static std::int32_t AMBROSIA_GUEST_ABI list_reset(void *pointer, Allocator *allocator,
                                                      const void *initial_state) noexcept {
        const PreserveErrno preserve;
        try {
            auto access = owned_list(pointer);
            if (!access.list.recording.closed()) {
                return static_cast<std::int32_t>(0x80004005u);
            }
            if (initial_state) {
                return static_cast<std::int32_t>(0x80004001u);
            }
            const auto found = access.state.allocators.find(allocator);
            if (found == access.state.allocators.end() || allocator->parent != access.list.parent ||
                allocator->type != access.list.type || allocator->recording) {
                return static_cast<std::int32_t>(0x80070057u);
            }
            if (allocator->references == UINT32_MAX) {
                diagnostics::stop_trace("native command allocator reference count exhausted", 78);
            }
            if (!access.list.recording.reset()) {
                return static_cast<std::int32_t>(0x80004005u);
            }
            ++allocator->references;
            allocator->recording = pointer;
            access.list.active_allocator = allocator;
            return 0;
        } catch (const std::exception &error) {
            diagnostics::stop_trace(error.what(), 78);
        }
    }

    template <std::size_t... Slots>
    static std::array<std::uintptr_t, sizeof...(Slots)> make_list_table(std::index_sequence<Slots...>) {
        return {reinterpret_cast<std::uintptr_t>(&unsupported_list<Slots>)...};
    }

    static const auto &list_table() {
        static const auto entries = [] {
            // Published console numbering extends well beyond the PC interface.
            // Unknown entries terminate; they never acknowledge a GPU command.
            auto result = make_list_table(std::make_index_sequence<600>{});
            result[0] = reinterpret_cast<std::uintptr_t>(&list_query);
            result[1] = reinterpret_cast<std::uintptr_t>(&list_add);
            result[2] = reinterpret_cast<std::uintptr_t>(&list_release);
            result[7] = reinterpret_cast<std::uintptr_t>(&list_device);
            result[8] = reinterpret_cast<std::uintptr_t>(&list_type);
            result[9] = reinterpret_cast<std::uintptr_t>(&list_close);
            result[10] = reinterpret_cast<std::uintptr_t>(&list_reset);
            return result;
        }();
        return entries;
    }

    static std::int32_t AMBROSIA_GUEST_ABI create_command_list_x(Device *pointer, const void *description,
                                                                 Allocator *allocator,
                                                                 const void *initial_state,
                                                                 const void *interface_id,
                                                                 void **output) noexcept {
        const PreserveErrno preserve;
        if (!output) {
            return pointer_error;
        }
        *output = nullptr;
        if (!description || !interface_id) {
            return pointer_error;
        }
        try {
            auto access = owned(pointer);
            if (!supports_list(interface_id)) {
                return no_interface;
            }
            std::array<std::uint32_t, 4> words;
            std::memcpy(words.data(), description, sizeof(words));
            const auto found = access.state.allocators.find(allocator);
            if (found == access.state.allocators.end() || allocator->parent != pointer || words[0] > 3 ||
                allocator->type != words[0] || allocator->recording) {
                return static_cast<std::int32_t>(0x80070057u);
            }
            if (initial_state) {
                return static_cast<std::int32_t>(0x80004001u);
            }
            if (access.state.lists.size() >= 1024 || allocator->references == UINT32_MAX ||
                pointer->references == UINT32_MAX) {
                diagnostics::stop_trace("native command-list ownership capacity exhausted", 78);
            }
            (void)access.state.backend->query_device(pointer->real.id);
            auto list = std::make_unique<List>(list_table().data());
            list->parent = pointer;
            list->type = words[0];
            list->description = words;
            auto *object = list->recording.guest_object();
            auto [position, inserted] = access.state.lists.emplace(object, std::move(list));
            if (!inserted) {
                throw std::runtime_error("native command-list object ownership collision");
            }
            ++pointer->references;
            ++allocator->references;
            allocator->recording = object;
            position->second->active_allocator = allocator;
            *output = object;
            char message[96];
            const auto count =
                std::snprintf(message, sizeof(message),
                              "TRACE native console CPU command list created type=%u\n", words[0]);
            if (count > 0 && static_cast<std::size_t>(count) < sizeof(message)) {
                diagnostics::write_service_trace(std::string_view(message, static_cast<std::size_t>(count)));
            }
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
            auto result = make_table(std::make_index_sequence<192>{});
            result[0] = reinterpret_cast<std::uintptr_t>(&query_interface);
            result[1] = reinterpret_cast<std::uintptr_t>(&add_reference);
            result[2] = reinterpret_cast<std::uintptr_t>(&release);
            result[7] = reinterpret_cast<std::uintptr_t>(&node_count);
            result[8] = reinterpret_cast<std::uintptr_t>(&create_queue);
            result[9] = reinterpret_cast<std::uintptr_t>(&create_allocator);
            result[26] = reinterpret_cast<std::uintptr_t>(&heap_properties);
            result[36] = reinterpret_cast<std::uintptr_t>(&create_fence);
            // The Xbox ABI shadows PC OpenExistingHeapFromAddress here.
            // See d3d12_xs_device.h and docs/retail_boot_chain.md section 3.2.
            result[48] = reinterpret_cast<std::uintptr_t>(&create_queue_x);
            result[73] = reinterpret_cast<std::uintptr_t>(&create_command_list_x);
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
