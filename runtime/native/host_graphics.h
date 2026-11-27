#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

namespace ambrosia::host {
struct GraphicsDevice {
    std::uint32_t id = 0;
    std::uint32_t api_version = 0;
};

struct GraphicsBuffer {
    std::uint32_t id = 0;
    std::uint64_t bytes = 0;
    std::uint64_t device_address = 0;
};

// The service owns Vulkan handles. This portable seam carries only bounded
// commands, numeric ownership IDs and GPU virtual addresses. Driver entrypoints
// and host CPU pointers remain private to the service.
class Graphics {
  public:
    explicit Graphics(const std::filesystem::path &endpoint_root);
    ~Graphics();
    Graphics(const Graphics &) = delete;
    Graphics &operator=(const Graphics &) = delete;
    GraphicsDevice create_device();
    GraphicsDevice query_device(std::uint32_t id);
    void destroy_device(std::uint32_t id);
    bool unified_memory();
    std::uint32_t create_fence(std::uint64_t initial, bool shared = false);
    std::uint64_t completed_fence(std::uint32_t id);
    void signal_fence(std::uint32_t id, std::uint64_t value);
    void destroy_fence(std::uint32_t id);
    GraphicsBuffer create_buffer(std::uint64_t bytes);
    void destroy_buffer(std::uint32_t id);
    void upload_buffer(std::uint32_t id, std::uint64_t offset, std::span<const std::byte> bytes);
    void download_buffer(std::uint32_t id, std::uint64_t offset, std::span<std::byte> bytes);
    void copy_buffer(std::uint32_t source, std::uint64_t source_offset, std::uint32_t destination,
                     std::uint64_t destination_offset, std::uint32_t bytes);

  private:
    struct Transport;
    std::unique_ptr<Transport> transport_;
};
} // namespace ambrosia::host
