#include "crt_startup.h"
#include <stdexcept>

namespace ambrosia::services {
void CrtStartup::bind_image(const std::filesystem::path &image) {
    if (bound_ || image.empty()) {
        throw std::runtime_error("CRT executable path is missing or already bound");
    }
    const auto utf8 = image.u8string();
    if (utf8.size() > 131068 || utf8.find(char8_t{}) != std::u8string::npos) {
        throw std::runtime_error("CRT executable path exceeds the bootstrap path policy");
    }
    image_.assign(reinterpret_cast<const char *>(utf8.data()), utf8.size());
    program_name_ = image_.data();
    arguments_[0] = image_.data();
    // Binding is one-shot: guest pointers into this owned path must remain
    // stable through module initialization, entry, callbacks and teardown.
    bound_ = true;
}

bool CrtStartup::set_app_type(std::int32_t type) noexcept {
    if (type < 0 || type > 2) {
        return false;
    }
    app_type_ = type;
    return true;
}

std::int32_t CrtStartup::set_file_mode(std::int32_t mode) noexcept {
    if (mode != 0x4000 && mode != 0x8000) {
        return 22;
    }
    file_mode_ = mode;
    return 0;
}

std::int32_t CrtStartup::configure_arguments(std::int32_t mode) noexcept {
    if (mode < 0 || mode > 2 || (!bound_ && mode != 0)) {
        return 22;
    }
    // The native CLI currently forwards no game arguments. argv[0] is never
    // wildcard-expanded, so expanded/unexpanded modes have the same one-item
    // result. Adding forwarded arguments requires actual parsing/expansion.
    argc_ = mode == 0 ? 0 : 1;
    argv_ = mode == 0 ? nullptr : arguments_.data();
    return 0;
}

std::int32_t CrtStartup::initialize_environment() noexcept {
    // The guest intentionally starts with an empty environment. These owned
    // NUL-terminated arrays never inherit credentials or host process options.
    if (!environment_) {
        environment_ = current_environment_.data();
    }
    return 0;
}
} // namespace ambrosia::services
