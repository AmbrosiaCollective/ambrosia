#include "gdk_threading.h"
#include "guest_thread.h"
#include "startup_trace.h"
#include "task_queues.h"
#include <array>
#include <string>

namespace ambrosia::services {
namespace {
// A nonreturning diagnostic endpoint needs no argument decoding or guessed
// return type. Unsupported slots never return an invented HRESULT/value.
template <unsigned Slot> [[noreturn]] void AMBROSIA_GUEST_ABI unsupported() noexcept {
    diagnostics::stop_trace("unsupported XThreadingImpl slot " + std::to_string(Slot), 78);
}

guest::ThreadContext &thread() {
    auto *context = guest::ThreadContext::current();
    if (!context) {
        diagnostics::stop_trace("GDK threading properties require an active guest context", 78);
    }
    return *context;
}

std::int32_t AMBROSIA_GUEST_ABI set_sensitive(void *, bool value) noexcept {
    thread().set_time_sensitive(value);
    return 0;
}

void AMBROSIA_GUEST_ABI assert_not_sensitive(void *) noexcept {
    if (thread().time_sensitive()) {
        diagnostics::stop_trace("guest thread is marked time-sensitive at a blocking boundary", 78);
    }
}

bool AMBROSIA_GUEST_ABI is_sensitive(void *) noexcept {
    return thread().time_sensitive();
}

std::int32_t AMBROSIA_GUEST_ABI create_queue(void *, std::uint32_t work, std::uint32_t completion,
                                             void *output) noexcept {
    return task_queue_create(work, completion, output);
}

std::int32_t AMBROSIA_GUEST_ABI duplicate_queue(void *, std::uintptr_t handle, void *output) noexcept {
    return task_queue_duplicate(handle, output);
}

void AMBROSIA_GUEST_ABI close_queue(void *, std::uintptr_t handle) noexcept {
    task_queue_close(handle);
}

const std::array<std::uintptr_t, 31> threading_vtable{
    reinterpret_cast<std::uintptr_t>(&interface_query),
    reinterpret_cast<std::uintptr_t>(&interface_add_reference),
    reinterpret_cast<std::uintptr_t>(&interface_release),
    reinterpret_cast<std::uintptr_t>(&unsupported<3>),
    reinterpret_cast<std::uintptr_t>(&unsupported<4>),
    reinterpret_cast<std::uintptr_t>(&unsupported<5>),
    reinterpret_cast<std::uintptr_t>(&unsupported<6>),
    reinterpret_cast<std::uintptr_t>(&unsupported<7>),
    reinterpret_cast<std::uintptr_t>(&unsupported<8>),
    reinterpret_cast<std::uintptr_t>(&unsupported<9>),
    reinterpret_cast<std::uintptr_t>(&unsupported<10>),
    reinterpret_cast<std::uintptr_t>(&unsupported<11>),
    reinterpret_cast<std::uintptr_t>(&create_queue),
    reinterpret_cast<std::uintptr_t>(&unsupported<13>),
    reinterpret_cast<std::uintptr_t>(&unsupported<14>),
    reinterpret_cast<std::uintptr_t>(&duplicate_queue),
    reinterpret_cast<std::uintptr_t>(&unsupported<16>),
    reinterpret_cast<std::uintptr_t>(&close_queue),
    reinterpret_cast<std::uintptr_t>(&unsupported<18>),
    reinterpret_cast<std::uintptr_t>(&unsupported<19>),
    reinterpret_cast<std::uintptr_t>(&unsupported<20>),
    reinterpret_cast<std::uintptr_t>(&unsupported<21>),
    reinterpret_cast<std::uintptr_t>(&unsupported<22>),
    reinterpret_cast<std::uintptr_t>(&unsupported<23>),
    reinterpret_cast<std::uintptr_t>(&unsupported<24>),
    reinterpret_cast<std::uintptr_t>(&unsupported<25>),
    reinterpret_cast<std::uintptr_t>(&unsupported<26>),
    reinterpret_cast<std::uintptr_t>(&set_sensitive),
    reinterpret_cast<std::uintptr_t>(&unsupported<28>),
    reinterpret_cast<std::uintptr_t>(&assert_not_sensitive),
    reinterpret_cast<std::uintptr_t>(&is_sensitive)};
} // namespace

GdkThreading::GdkThreading() : object_(threading_vtable.data(), threading_class_id) {}

std::int32_t GdkThreading::query(const Guid &interface_id, void *output) noexcept {
    return interface_query(&object_, &interface_id, output);
}
} // namespace ambrosia::services
