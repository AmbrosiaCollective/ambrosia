#pragma once
#include "host_readonly_files.h"
#include "import_registry.h"
#include "kernel_objects.h"
#include <condition_variable>

namespace ambrosia::services {
class ReadonlyFileObject final : public WaitableObject {
  public:
    ReadonlyFileObject(std::shared_ptr<host::ReadonlyFile> file, bool read_data)
        : file_(std::move(file)), read_data_(read_data) {}

    WaitResult wait(std::uint32_t milliseconds) override;
    host::FileLookup inspect() const;
    host::FileTimes times() const;
    host::FileRead read(std::span<std::byte> output);

    bool read_data() const noexcept {
        return read_data_;
    }

  private:
    std::shared_ptr<host::ReadonlyFile> file_;
    bool read_data_;
    std::mutex io_mutex_;
    std::condition_variable io_completed_;
    bool busy_ = false;
    std::uint64_t position_ = 0;
};

std::optional<std::uint64_t> windows_filetime(host::FileTimestamp timestamp) noexcept;

void register_file_services(ImportRegistry &registry);
} // namespace ambrosia::services
