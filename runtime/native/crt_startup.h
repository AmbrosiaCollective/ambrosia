#pragma once
#include "import_registry.h"
#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

namespace ambrosia::services {
// Fixed Windows x64 launch-record ABI. Explicit alignment fields keep every
// byte initialized when copied to guest memory; no native desktop/FDs leak in.
struct StartupInfo64 {
    std::uint32_t size = 104;
    std::uint32_t alignment0 = 0;
    std::uint64_t reserved = 0;
    std::uint64_t desktop = 0;
    std::uint64_t title = 0;
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t columns = 0;
    std::uint32_t rows = 0;
    std::uint32_t fill_attribute = 0;
    std::uint32_t flags = 0;
    std::uint16_t show_window = 0;
    std::uint16_t reserved_size = 0;
    std::uint32_t alignment1 = 0;
    std::uint64_t reserved_data = 0;
    std::uint64_t standard_input = 0;
    std::uint64_t standard_output = 0;
    std::uint64_t standard_error = 0;
};

static_assert(sizeof(StartupInfo64) == 104);
static_assert(offsetof(StartupInfo64, flags) == 60);
static_assert(offsetof(StartupInfo64, show_window) == 64);
static_assert(offsetof(StartupInfo64, standard_input) == 80);

class CrtStartup {
  public:
    CrtStartup() = default;
    CrtStartup(const CrtStartup &) = delete;
    CrtStartup &operator=(const CrtStartup &) = delete;
    void bind_image(const std::filesystem::path &image);
    bool set_app_type(std::int32_t type) noexcept;
    bool set_show_window(std::optional<std::uint16_t> mode) noexcept;

    const StartupInfo64 &launch_info() const noexcept {
        return launch_info_;
    }

    std::int32_t show_window_mode() const noexcept {
        return launch_info_.flags & 1 ? launch_info_.show_window : 10; // SW_SHOWDEFAULT.
    }

    std::int32_t app_type() const noexcept {
        return app_type_;
    }

    std::int32_t set_file_mode(std::int32_t mode) noexcept;
    std::int32_t configure_arguments(std::int32_t mode) noexcept;
    std::int32_t initialize_environment() noexcept;

    std::int32_t *argument_count() noexcept {
        return &argc_;
    }

    char ***argument_vector() noexcept {
        return &argv_;
    }

    char ***environment() noexcept {
        return &environment_;
    }

    char **initial_environment() noexcept {
        (void)initialize_environment();
        return initial_environment_.data();
    }

    std::int32_t *file_mode() noexcept {
        return &file_mode_;
    }

    std::uint32_t *commit_mode() noexcept {
        return &commit_mode_;
    }

    char **program_name() noexcept {
        return &program_name_;
    }

    char *winmain_arguments() noexcept {
        return empty_arguments_.data();
    }

    char *command_line_a() noexcept {
        return bound_ ? command_line_a_.data() : nullptr;
    }

    char16_t *command_line_w() noexcept {
        return bound_ ? command_line_w_.data() : nullptr;
    }

  private:
    StartupInfo64 launch_info_;
    std::string image_;
    std::string command_line_a_;
    std::u16string command_line_w_;
    bool bound_ = false;
    std::int32_t app_type_ = 0;
    std::int32_t file_mode_ = 0x4000; // Guest _O_TEXT, not a host fcntl value.
    std::uint32_t commit_mode_ = 0;
    std::int32_t argc_ = 0;
    char **argv_ = nullptr;
    char *program_name_ = nullptr;
    char **environment_ = nullptr;
    std::array<char *, 2> arguments_{};
    std::array<char *, 1> current_environment_{};
    std::array<char *, 1> initial_environment_{};
    std::array<char, 1> empty_arguments_{};
};

void register_crt_startup(ImportRegistry &registry);
} // namespace ambrosia::services
