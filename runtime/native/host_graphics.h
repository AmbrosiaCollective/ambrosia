#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>

namespace ambrosia::host {
struct GraphicsDevice {
    std::uint32_t id = 0;
    std::uint32_t api_version = 0;
};

// The service owns Vulkan handles. This portable seam carries only bounded
// commands and numeric ownership IDs, never driver entrypoints or GPU pointers.
class Graphics {
  public:
    explicit Graphics(const std::filesystem::path &endpoint_root);
    ~Graphics();
    Graphics(const Graphics &) = delete;
    Graphics &operator=(const Graphics &) = delete;
    GraphicsDevice create_device();
    GraphicsDevice query_device(std::uint32_t id);
    void destroy_device(std::uint32_t id);

  private:
    struct Transport;
    std::unique_ptr<Transport> transport_;
};
} // namespace ambrosia::host
