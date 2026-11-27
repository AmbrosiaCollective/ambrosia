#include "graphics_recording.h"
#include "command_list.h"
#include "host.h"
#include <array>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace ambrosia::services {
struct GraphicsRecording::State {
    // Guest inline caches may write these bytes. The IR and C++ ownership are
    // deliberately outside this mapping so those writes cannot corrupt them.
    host::Mapping object{console_list_object_bytes};
    CommandList commands;
    std::vector<RecordedCacheBarrier> cache_barriers;

    void store(std::size_t offset, std::uintptr_t value) {
        std::memcpy(object.data() + offset, &value, sizeof(value));
    }

    std::uintptr_t load(std::size_t offset) const {
        std::uintptr_t value;
        std::memcpy(&value, object.data() + offset, sizeof(value));
        return value;
    }

    std::uintptr_t room() const {
        return reinterpret_cast<std::uintptr_t>(object.data() + console_list_room_offset);
    }

    void install_bounds() {
        for (const auto offset :
             {console_list_write_offset, console_list_dispatch_end_offset, console_list_end_offset}) {
            store(offset, room());
        }
    }

    bool bounds_intact() const {
        return load(console_list_write_offset) == room() &&
               load(console_list_dispatch_end_offset) == room() && load(console_list_end_offset) == room();
    }

    void require_bounds() const {
        if (!bounds_intact()) {
            throw std::runtime_error(
                "native console command-list buffer bounds changed before IR transition");
        }
    }
};

GraphicsRecording::GraphicsRecording(const std::uintptr_t *table) : state_(std::make_unique<State>()) {
    static_assert(sizeof(std::uintptr_t) == 8);
    static_assert(console_list_end_offset + sizeof(std::uintptr_t) < console_list_room_offset);
    static_assert(console_list_room_offset < console_list_object_bytes);
    if (!table) {
        throw std::invalid_argument("native console command list requires a table");
    }
    state_->store(0, reinterpret_cast<std::uintptr_t>(table));
    state_->install_bounds();
}

GraphicsRecording::~GraphicsRecording() = default;

void *GraphicsRecording::guest_object() const {
    return state_->object.data();
}

bool GraphicsRecording::bounds_intact() const {
    return state_->bounds_intact();
}

bool GraphicsRecording::close() {
    state_->require_bounds();
    return state_->commands.close();
}

bool GraphicsRecording::reset() {
    state_->require_bounds();
    if (!state_->commands.reset()) {
        return false;
    }
    state_->install_bounds();
    state_->cache_barriers.clear();
    return true;
}

bool GraphicsRecording::closed() const {
    return state_->commands.closed();
}

bool GraphicsRecording::cache_barrier(const ConsoleCacheBarrier &request) {
    state_->require_bounds();
    if (closed() || request.address > UINT64_MAX - request.bytes ||
        state_->cache_barriers.size() >= console_cache_barrier_limit) {
        return false;
    }
    // Adapt TuxBox's full-memory fallback, not its Windows COM implementation.
    // Keep both masks and the range for future narrower lowering. See REUSE.md.
    const GlobalBarrier barrier{d3d12_sync::all, d3d12_sync::all, d3d12_access::common, d3d12_access::common};
    state_->cache_barriers.push_back({request, state_->commands.commands().size()});
    try {
        if (state_->commands.barrier(barrier)) {
            return true;
        }
    } catch (...) {
        state_->cache_barriers.pop_back();
        throw;
    }
    state_->cache_barriers.pop_back();
    return false;
}

std::span<const RecordedCacheBarrier> GraphicsRecording::cache_barriers() const {
    return state_->cache_barriers;
}

CommandList &GraphicsRecording::command_ir() {
    return state_->commands;
}

const CommandList &GraphicsRecording::command_ir() const {
    return state_->commands;
}
} // namespace ambrosia::services
