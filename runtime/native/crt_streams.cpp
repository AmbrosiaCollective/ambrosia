#include "crt_streams.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include <cstring>

namespace ambrosia::services {
namespace {
void *AMBROSIA_GUEST_ABI standard_stream(unsigned index) noexcept {
    auto *runtime = current_runtime();
    if (!runtime) {
        diagnostics::stop_trace("CRT stream accessed without scoped runtime", 78);
    }
    auto *file = runtime->streams().file(index);
    if (!file) {
        diagnostics::stop_trace("unsupported CRT standard-stream index", 78);
    }
    return file;
}

std::int32_t AMBROSIA_GUEST_ABI stream_buffer(void *file, void *base_output, void *cursor_output,
                                              void *count_output) noexcept {
    auto *runtime = current_runtime();
    if (!runtime) {
        diagnostics::stop_trace("CRT stream buffer accessed without scoped runtime", 78);
    }
    auto *buffer = runtime->streams().buffer(file);
    if (!buffer) {
        crt_error() = 22;
        return 22;
    }
    // These are addresses of mutable guest stream fields, not copies of their
    // current values. Keep them stable for the lifetime of the scoped runtime.
    if (base_output) {
        auto **base = &buffer->base;
        std::memcpy(base_output, &base, sizeof(base));
    }
    if (cursor_output) {
        auto **cursor = &buffer->cursor;
        std::memcpy(cursor_output, &cursor, sizeof(cursor));
    }
    if (count_output) {
        auto *count = &buffer->remaining;
        std::memcpy(count_output, &count, sizeof(count));
    }
    return 0;
}
} // namespace

void register_crt_streams(ImportRegistry &registry) {
    for (const auto *module : {"ucrtbase.dll", "api-ms-win-crt-stdio-l1-1-0.dll"}) {
        registry.add(module, "__acrt_iob_func", reinterpret_cast<std::uintptr_t>(&standard_stream));
        registry.add(module, "_get_stream_buffer_pointers", reinterpret_cast<std::uintptr_t>(&stream_buffer));
    }
}
} // namespace ambrosia::services
