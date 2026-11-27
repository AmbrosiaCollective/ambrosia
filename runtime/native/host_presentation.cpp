#include "host_presentation.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#ifdef __linux__
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ambrosia::host {
namespace {
constexpr std::array<char, 8> protocol_magic{'A', 'M', 'B', 'W', 'I', 'N', '0', '1'};
constexpr std::size_t pixel_limit = 3840 * 2160;
constexpr std::size_t payload_limit = 4096;

struct Packet {
    std::array<char, 8> magic = protocol_magic;
    std::uint32_t sequence = 0, operation = 0, window = 0, payload_bytes = 0;
    WindowGeometry geometry;
    std::uint32_t flags = 0, argument = 0, reserved1 = 0, reserved2 = 0;
    std::array<std::byte, 8> padding{};
};

static_assert(sizeof(Packet) == 64 && offsetof(Packet, flags) == 40);
static_assert(sizeof(PresentationEvent) == 24);
static_assert(std::endian::native == std::endian::little);

std::size_t frame_bytes(std::int32_t width, std::int32_t height) {
    if (width <= 0 || height <= 0 || width > 4096 || height > 2160 ||
        static_cast<std::size_t>(width) * height > pixel_limit) {
        throw std::runtime_error("native presentation dimensions exceed the bounded frame budget");
    }
    return static_cast<std::size_t>(width) * height * 4;
}

WindowVisibility visibility_reply(const Packet &reply, std::uint32_t window) {
    if (reply.window != window || reply.argument > 3) {
        throw std::runtime_error("invalid native owned window visibility reply");
    }
    (void)frame_bytes(reply.geometry.width, reply.geometry.height);
    return {reply.geometry, bool(reply.argument & 1), bool(reply.argument & 2)};
}

#ifdef __linux__
void transfer_exact(int descriptor, void *data, std::size_t bytes, std::size_t offset, bool write) {
    auto *cursor = static_cast<std::byte *>(data);
    while (bytes) {
        const auto transferred = write ? pwrite(descriptor, cursor, bytes, static_cast<off_t>(offset))
                                       : pread(descriptor, cursor, bytes, static_cast<off_t>(offset));
        if (transferred < 0 && errno == EINTR) {
            continue;
        }
        if (transferred <= 0) {
            throw std::runtime_error("native presentation endpoint transfer failed or was truncated");
        }
        const auto count = static_cast<std::size_t>(transferred);
        cursor += count;
        offset += count;
        bytes -= count;
    }
}
#endif
} // namespace

struct Presentation::Transport {
    std::mutex mutex;
    std::uint32_t sequence = 0;
    int request = -1, response = -1, frame = -1;

    ~Transport() {
#ifdef __linux__
        for (const auto descriptor : {request, response, frame}) {
            if (descriptor >= 0) {
                (void)close(descriptor);
            }
        }
#endif
    }

    Packet exchange(Packet packet, std::string_view payload = {},
                    std::vector<PresentationEvent> *events = nullptr) {
#ifdef __linux__
        if (sequence == UINT32_MAX || payload.size() > payload_limit) {
            throw std::runtime_error("native presentation sequence or payload budget exhausted");
        }
        packet.sequence = ++sequence;
        packet.payload_bytes = static_cast<std::uint32_t>(payload.size());
        if (!payload.empty()) {
            transfer_exact(request, const_cast<char *>(payload.data()), payload.size(), sizeof(Packet), true);
        }
        // Commit the sequence-bearing header last; the broker copies/validates
        // the payload and checks the header again before interpreting it.
        transfer_exact(request, &packet, sizeof(packet), 0, true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        Packet reply;
        for (;;) {
            transfer_exact(response, &reply, sizeof(reply), 0, false);
            if (reply.sequence == packet.sequence) {
                break;
            }
            if (reply.sequence > packet.sequence || std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error("native presentation response sequence or deadline failed");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (reply.magic != protocol_magic || reply.operation != packet.operation || reply.reserved1 ||
            reply.reserved2 || reply.padding != std::array<std::byte, 8>{} ||
            reply.payload_bytes > payload_limit) {
            throw std::runtime_error("invalid native presentation response header");
        }
        if (reply.flags) {
            throw std::runtime_error("native presentation service rejected the bounded request");
        }
        if (events) {
            if (reply.argument > 64 || reply.payload_bytes != reply.argument * sizeof(PresentationEvent)) {
                throw std::runtime_error("invalid native presentation event count");
            }
            events->resize(reply.argument);
            if (!events->empty()) {
                transfer_exact(response, events->data(), reply.payload_bytes, sizeof(Packet), false);
            }
            if (std::any_of(events->begin(), events->end(), [](const auto &event) {
                    return !event.window || !event.kind || event.kind > 9;
                })) {
                throw std::runtime_error("invalid native presentation event metadata");
            }
        } else if (reply.payload_bytes ||
                   (packet.operation == 8 ? reply.argument > 3 : reply.argument != 0)) {
            throw std::runtime_error("unexpected native presentation payload");
        }
        return reply;
#else
        (void)packet;
        (void)payload;
        (void)events;
        throw std::runtime_error("native presentation file transport is unsupported on this host");
#endif
    }
};

Presentation::Presentation(const std::filesystem::path &endpoint_root)
    : transport_(std::make_unique<Transport>()) {
#ifdef __linux__
    const auto directory = open(endpoint_root.c_str(), O_DIRECTORY | O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0) {
        throw std::runtime_error("cannot open explicit native presentation endpoints");
    }
    try {
        const auto endpoint = [&](const char *name, int access) {
            const auto descriptor = openat(directory, name, access | O_CLOEXEC | O_NOFOLLOW);
            if (descriptor < 0) {
                throw std::runtime_error("cannot open native presentation endpoint");
            }
            struct stat information {};
            if (fstat(descriptor, &information) || !S_ISREG(information.st_mode) ||
                information.st_nlink != 1 || information.st_size < 0 ||
                information.st_size > 32 * 1024 * 1024) {
                (void)close(descriptor);
                throw std::runtime_error("invalid native presentation endpoint type or size");
            }
            return descriptor;
        };
        transport_->request = endpoint("request", O_RDWR);
        transport_->response = endpoint("response", O_RDONLY);
        transport_->frame = endpoint("frame", O_WRONLY);
        if (flock(transport_->request, LOCK_EX | LOCK_NB)) {
            throw std::runtime_error("native presentation endpoints already have a client owner");
        }
        Packet last;
        transfer_exact(transport_->response, &last, sizeof(last), 0, false);
        if (last.magic == protocol_magic) {
            transport_->sequence = last.sequence;
        } else {
            const auto *bytes = reinterpret_cast<const std::byte *>(&last);
            if (std::any_of(bytes, bytes + sizeof(last), [](auto byte) { return byte != std::byte{0}; })) {
                throw std::runtime_error("invalid initial native presentation response");
            }
        }
        (void)close(directory);
    } catch (...) {
        (void)close(directory);
        throw;
    }
#else
    (void)endpoint_root;
    throw std::runtime_error("native presentation file transport is unsupported on this host");
#endif
}

Presentation::~Presentation() = default;

PresentationWindow Presentation::create(const WindowRequest &request) {
    (void)frame_bytes(request.geometry.width, request.geometry.height);
    if (request.title.size() > payload_limit || request.title.find('\0') != std::string::npos) {
        throw std::runtime_error("native presentation title exceeds its string contract");
    }
    const std::lock_guard lock(transport_->mutex);
    Packet packet;
    packet.operation = 1;
    packet.geometry = request.geometry;
    packet.flags =
        (request.visible ? 1u : 0u) | (request.resizable ? 2u : 0u) | (request.borderless ? 4u : 0u);
    const auto reply = transport_->exchange(packet, request.title);
    if (!reply.window) {
        throw std::runtime_error("native presentation service returned no owned window");
    }
    return {reply.window, reply.geometry};
}

void Presentation::destroy(std::uint32_t window) {
    const std::lock_guard lock(transport_->mutex);
    Packet packet;
    packet.operation = 2;
    packet.window = window;
    const auto reply = transport_->exchange(packet);
    if (reply.window != window) {
        throw std::runtime_error("native presentation destroyed a different window");
    }
}

WindowGeometry Presentation::show(std::uint32_t window, bool visible) {
    const std::lock_guard lock(transport_->mutex);
    Packet packet;
    packet.operation = 3;
    packet.window = window;
    packet.flags = visible ? 1 : 0;
    const auto reply = transport_->exchange(packet);
    if (reply.window != window) {
        throw std::runtime_error("native presentation visibility refers to a different window");
    }
    return reply.geometry;
}

std::vector<PresentationEvent> Presentation::poll() {
    const std::lock_guard lock(transport_->mutex);
    Packet packet;
    packet.operation = 4;
    std::vector<PresentationEvent> events;
    (void)transport_->exchange(packet, {}, &events);
    return events;
}

void Presentation::set_title(std::uint32_t window, std::string_view title) {
    if (title.size() > payload_limit || title.find('\0') != std::string_view::npos) {
        throw std::runtime_error("native presentation title exceeds its string contract");
    }
    const std::lock_guard lock(transport_->mutex);
    Packet packet;
    packet.operation = 6;
    packet.window = window;
    const auto reply = transport_->exchange(packet, title);
    if (reply.window != window) {
        throw std::runtime_error("native presentation title refers to a different window");
    }
}

WindowBorders Presentation::borders(std::uint32_t window) {
    const std::lock_guard lock(transport_->mutex);
    Packet packet;
    packet.operation = 7;
    packet.window = window;
    const auto reply = transport_->exchange(packet);
    const auto &values = reply.geometry;
    if (reply.window != window || values.x < 0 || values.y < 0 || values.width < 0 || values.height < 0 ||
        values.x > 4096 || values.y > 4096 || values.width > 4096 || values.height > 4096) {
        throw std::runtime_error("native presentation returned invalid owned window borders");
    }
    return {values.x, values.y, values.width, values.height};
}

WindowVisibility Presentation::visibility(std::uint32_t window) {
    const std::lock_guard lock(transport_->mutex);
    Packet packet;
    packet.operation = 8;
    packet.window = window;
    packet.flags = 2; // Query without changing the host window.
    const auto result = visibility_reply(transport_->exchange(packet), window);
    if (result.previous != result.visible) {
        throw std::runtime_error("native visibility query unexpectedly changed state");
    }
    return result;
}

WindowVisibility Presentation::set_visibility(std::uint32_t window, bool visible, bool normal) {
    if (normal && !visible) {
        throw std::invalid_argument("native normal visibility requires showing the window");
    }
    const std::lock_guard lock(transport_->mutex);
    Packet packet;
    packet.operation = 8;
    packet.window = window;
    packet.flags = normal ? 3 : (visible ? 1 : 0);
    const auto result = visibility_reply(transport_->exchange(packet), window);
    if (result.visible != visible) {
        throw std::runtime_error("native visibility acknowledgment disagrees with requested state");
    }
    return result;
}

void Presentation::present(std::uint32_t window, std::int32_t width, std::int32_t height,
                           std::span<const std::byte> rgba) {
    const auto expected_bytes = frame_bytes(width, height);
    if (rgba.size() != expected_bytes) {
        throw std::runtime_error("native presentation frame size does not match its dimensions");
    }
    const std::lock_guard lock(transport_->mutex);
#ifdef __linux__
    if (ftruncate(transport_->frame, static_cast<off_t>(rgba.size()))) {
        throw std::runtime_error("cannot size bounded native presentation frame");
    }
    transfer_exact(transport_->frame, const_cast<std::byte *>(rgba.data()), rgba.size(), 0, true);
#endif
    Packet packet;
    packet.operation = 5;
    packet.window = window;
    packet.geometry.width = width;
    packet.geometry.height = height;
    const auto reply = transport_->exchange(packet);
    if (reply.window != window) {
        throw std::runtime_error("native presentation frame refers to a different window");
    }
}
} // namespace ambrosia::host
