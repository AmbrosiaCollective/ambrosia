#include "host.h"
#include <random>
#include <stdexcept>
#ifdef __linux__
#include <sys/random.h>
#endif

namespace ambrosia::host {
std::uint32_t secure_random_u32() {
#ifdef __linux__
    std::uint32_t value;
    // getentropy fills the entire small buffer or fails, including retries for
    // interrupted kernel reads. Never substitute a PRNG or pointer secret.
    if (getentropy(&value, sizeof(value))) {
        throw std::runtime_error("OS cryptographic entropy is unavailable");
    }
    return value;
#else
    throw std::runtime_error("cryptographic entropy adapter is unsupported on this host");
#endif
}

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
