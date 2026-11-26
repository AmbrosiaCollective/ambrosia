#pragma once
#include "host_readonly_files.h"
#include "import_registry.h"
#include "kernel_objects.h"

namespace ambrosia::services {
class ReadonlyFileObject final : public WaitableObject {
  public:
    ReadonlyFileObject(std::shared_ptr<host::ReadonlyFile> file, bool read_data)
        : file_(std::move(file)), read_data_(read_data) {}

    WaitResult wait(std::uint32_t milliseconds) override;
    host::FileLookup inspect() const;

    bool read_data() const noexcept {
        return read_data_;
    }

  private:
    std::shared_ptr<host::ReadonlyFile> file_;
    bool read_data_;
};

void register_file_services(ImportRegistry &registry);
} // namespace ambrosia::services
