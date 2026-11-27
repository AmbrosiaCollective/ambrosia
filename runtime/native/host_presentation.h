#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ambrosia::host {
struct WindowGeometry {
    std::int32_t x = 0, y = 0, width = 0, height = 0;
};

struct PresentationWindow {
    std::uint32_t id = 0;
    WindowGeometry geometry;
};

struct WindowRequest {
    std::string title;
    WindowGeometry geometry;
    bool visible = false;
    bool resizable = false;
    bool borderless = false;
};

struct PresentationEvent {
    std::uint32_t kind = 0, window = 0, flags = 0, detail = 0;
    std::int32_t first = 0, second = 0;
};

// A portable host seam. The Linux file transport talks only to an explicitly
// supplied, separately reviewed presentation service; it opens no display/socket.
class Presentation {
  public:
    explicit Presentation(const std::filesystem::path &endpoint_root);
    ~Presentation();
    Presentation(const Presentation &) = delete;
    Presentation &operator=(const Presentation &) = delete;
    PresentationWindow create(const WindowRequest &request);
    void destroy(std::uint32_t window);
    WindowGeometry show(std::uint32_t window, bool visible);
    std::vector<PresentationEvent> poll();
    void present(std::uint32_t window, std::int32_t width, std::int32_t height,
                 std::span<const std::byte> rgba);

  private:
    struct Transport;
    std::unique_ptr<Transport> transport_;
};
} // namespace ambrosia::host
