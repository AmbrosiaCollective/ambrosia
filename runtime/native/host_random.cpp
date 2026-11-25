#include "host.h"
#include <random>
#include <stdexcept>

namespace ambrosia::host {
std::uintptr_t random_pointer_secret() {
    std::random_device source;
    if (source.entropy() <= 0) {
        throw std::runtime_error("host nondeterministic entropy is unavailable");
    }
    std::uniform_int_distribution<std::uintptr_t> sample;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        const auto secret = sample(source);
        if (secret) {
            return secret;
        }
    }
    throw std::runtime_error("host entropy did not produce a nonzero pointer secret");
}
} // namespace ambrosia::host
