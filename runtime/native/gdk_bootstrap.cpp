#include "gdk_bootstrap.h"
#include <limits>

namespace ambrosia::services {
std::int32_t GdkBootstrap::initialize(std::uint32_t gdk_version, std::uint32_t service_version) {
    if (!gdk_version) {
        return gdk_invalid_argument;
    }
    const auto version = std::make_pair(gdk_version, service_version);
    const std::lock_guard guard(mutex_);
    if (references_ == 4096 || (!versions_.contains(version) && versions_.size() == 64)) {
        return gdk_out_of_memory;
    }
    // Different libraries may use different SDK revisions. Recording a revision
    // does not claim all its interfaces: QueryApiImpl must provide each one.
    versions_.insert(version);
    ++references_;
    return 0;
}

void GdkBootstrap::uninitialize() {
    const std::lock_guard guard(mutex_);
    if (references_ && !--references_) {
        versions_.clear();
    }
}

std::int32_t GdkBootstrap::report_error(std::int32_t code, std::string message) {
    if (message.size() > 32768) {
        return gdk_invalid_argument;
    }
    const std::lock_guard guard(mutex_);
    last_message_.swap(message);
    last_error_ = code;
    if (reports_ != std::numeric_limits<std::uint64_t>::max()) {
        ++reports_;
    }
    return 0;
}

GdkBootstrapSnapshot GdkBootstrap::snapshot() const {
    const std::lock_guard guard(mutex_);
    return {references_, versions_.size(), reports_, last_error_};
}

std::string GdkBootstrap::last_error_message() const {
    const std::lock_guard guard(mutex_);
    return last_message_;
}
} // namespace ambrosia::services
