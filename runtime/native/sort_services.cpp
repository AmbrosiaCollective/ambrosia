#include "sort_services.h"
#include "guest_thread.h"
#include "service_runtime.h"
#include "startup_trace.h"
#include <algorithm>
#include <cerrno>
#include <limits>
#include <stdexcept>

namespace ambrosia::services {
namespace {
using Compare = std::int32_t(AMBROSIA_GUEST_ABI *)(const void *, const void *);
constexpr std::size_t record_limit = 1024 * 1024;
constexpr std::size_t byte_limit = 64 * 1024 * 1024;

struct RestoreNativeError {
    int saved = errno;

    ~RestoreNativeError() {
        errno = saved;
    }
};

class SortableRecords {
  public:
    SortableRecords(void *base, std::size_t width, Compare compare)
        : base_(static_cast<std::byte *>(base)), width_(width), compare_(compare) {}

    void sort(std::size_t count) {
        // In-place heapsort bounds work without allocation or recursion. Equal
        // records have unspecified order, as required by the qsort contract.
        for (auto start = count / 2; start > 0; --start) {
            sift_down(start - 1, count);
        }
        for (auto remaining = count; remaining > 1; --remaining) {
            swap_records(0, remaining - 1);
            sift_down(0, remaining - 1);
        }
    }

  private:
    std::byte *element(std::size_t index) const {
        return base_ + index * width_;
    }

    bool less(std::size_t left, std::size_t right) const {
        // Invoke the guest ABI directly on the caller's existing TLS/module
        // scope. A host libc callback or a fresh worker context is incompatible.
        return compare_(element(left), element(right)) < 0;
    }

    void swap_records(std::size_t left, std::size_t right) {
        std::swap_ranges(element(left), element(left) + width_, element(right));
    }

    void sift_down(std::size_t parent, std::size_t count) {
        while (parent < count / 2) {
            auto child = parent * 2 + 1;
            if (child + 1 < count && less(child, child + 1)) {
                ++child;
            }
            if (!less(parent, child)) {
                return;
            }
            swap_records(parent, child);
            parent = child;
        }
    }

    std::byte *base_;
    std::size_t width_;
    Compare compare_;
};

void AMBROSIA_GUEST_ABI sort_records(void *base, std::size_t count, std::size_t width,
                                     Compare compare) noexcept {
    RestoreNativeError restore;
    try {
        if (!current_runtime()) {
            throw std::runtime_error("CRT sorting requires a scoped runtime");
        }
        if ((!base && count) || !width || !compare) {
            crt_error() = 22;
            dispatch_invalid_parameter();
            return;
        }
        if (count < 2) {
            return;
        }
        if (count > record_limit || width > byte_limit / count) {
            throw std::runtime_error("CRT sorting exceeds the record or byte budget");
        }
        const auto bytes = count * width;
        if (reinterpret_cast<std::uintptr_t>(base) > std::numeric_limits<std::uintptr_t>::max() - bytes) {
            throw std::runtime_error("CRT sorting array address wraps");
        }
        const auto *graph = current_modules();
        if (!guest::ThreadContext::current() || !graph ||
            !graph->module_base_for_address(reinterpret_cast<std::uintptr_t>(compare))) {
            throw std::runtime_error("CRT sorting requires an active guest and mapped comparator");
        }
        SortableRecords(base, width, compare).sort(count);
        diagnostics::write_service_trace("TRACE native CRT records sorted\n");
    } catch (const std::exception &error) {
        diagnostics::stop_trace(error.what(), 78);
    } catch (...) {
        diagnostics::stop_trace("guest sorting exception propagation is unsupported", 78);
    }
}
} // namespace

void register_sort_services(ImportRegistry &registry) {
    for (const auto *module : {"ucrtbase.dll", "api-ms-win-crt-utility-l1-1-0.dll"}) {
        registry.add(module, "qsort", reinterpret_cast<std::uintptr_t>(&sort_records));
    }
}
} // namespace ambrosia::services
