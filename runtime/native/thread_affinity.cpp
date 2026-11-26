#include "thread_affinity.h"
#include <algorithm>
#include <set>
#include <stdexcept>

namespace ambrosia::services {
namespace {
std::uint32_t affinity_error(host::SchedulingError error) {
    switch (error) {
    case host::SchedulingError::none:
        return 0;
    case host::SchedulingError::denied:
        return 5;
    case host::SchedulingError::missing_thread:
        return 6;
    case host::SchedulingError::invalid:
        return 87;
    case host::SchedulingError::unsupported:
        return 50;
    case host::SchedulingError::failure:
        return 31;
    }
    return 31;
}
} // namespace

ThreadAffinity::ThreadAffinity(std::span<const unsigned> processors) : processors_(processors) {
    if (processors.empty() || processors.size() > 64 ||
        std::set<unsigned>(processors.begin(), processors.end()).size() != processors.size()) {
        throw std::runtime_error("thread affinity requires the saved dense single-group CPU mapping");
    }
}

std::uint32_t ThreadAffinity::read(std::uint32_t native_id, GroupAffinity &output) const {
    const auto actual = host::thread_processors(native_id);
    if (actual.error != host::SchedulingError::none) {
        return affinity_error(actual.error);
    }
    GroupAffinity result;
    for (const auto processor : actual.ids) {
        const auto found = std::find(processors_.begin(), processors_.end(), processor);
        if (found == processors_.end()) {
            return 50; // A changed/out-of-catalog mask must not be misreported.
        }
        result.mask |= std::uint64_t{1} << static_cast<unsigned>(found - processors_.begin());
    }
    output = result;
    return 0;
}

std::uint32_t ThreadAffinity::write(std::uint32_t native_id, const GroupAffinity &value,
                                    GroupAffinity &previous) const {
    const auto active = UINT64_MAX >> (64 - processors_.size());
    if (value.group || !value.mask || (value.mask & ~active) ||
        std::any_of(value.reserved.begin(), value.reserved.end(),
                    [](auto reserved) { return reserved != 0; })) {
        return 87;
    }
    GroupAffinity before;
    const auto read_error = read(native_id, before);
    if (read_error) {
        return read_error;
    }
    std::vector<unsigned> requested;
    for (std::size_t bit = 0; bit < processors_.size(); ++bit) {
        if (value.mask & (std::uint64_t{1} << bit)) {
            requested.push_back(processors_[bit]);
        }
    }
    std::sort(requested.begin(), requested.end());
    const auto error = affinity_error(host::set_thread_processors(native_id, requested));
    if (!error) {
        previous = before;
    }
    return error;
}
} // namespace ambrosia::services
