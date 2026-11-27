#include "host_graphics.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#ifdef __linux__
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ambrosia::host {
namespace {
constexpr std::array<char, 8> magic{'A', 'M', 'B', 'G', 'F', 'X', '0', '1'};
constexpr std::uint32_t minimum_api = (1u << 22) | (3u << 12);
constexpr std::uint64_t maximum_buffer_bytes = 256ull * 1024 * 1024;
constexpr std::size_t chunk_bytes = 1024 * 1024;

struct Packet {
    std::array<char, 8> signature = magic;
    std::uint32_t sequence = 0, operation = 0, owner = 0;
    std::uint32_t payload = 0;
    std::uint64_t value = 0;
    std::array<std::uint32_t, 2> unused{};
    std::uint32_t status = 0, api_version = 0;
    std::array<std::uint32_t, 4> reserved{};
};

static_assert(sizeof(Packet) == 64 && offsetof(Packet, status) == 40 && offsetof(Packet, value) == 24);
static_assert(std::endian::native == std::endian::little);

#ifdef __linux__
void transfer(int descriptor, Packet &packet, bool write) {
    auto *cursor = reinterpret_cast<std::byte *>(&packet);
    std::size_t offset = 0;
    while (offset < sizeof(packet)) {
        const auto count =
            write ? pwrite(descriptor, cursor + offset, sizeof(packet) - offset, static_cast<off_t>(offset))
                  : pread(descriptor, cursor + offset, sizeof(packet) - offset, static_cast<off_t>(offset));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            throw std::runtime_error("native graphics endpoint transfer failed or was truncated");
        }
        offset += static_cast<std::size_t>(count);
    }
}

void transfer_bytes(int descriptor, std::span<std::byte> bytes, bool write) {
    struct stat information {};

    if (fstat(descriptor, &information) || information.st_size != static_cast<off_t>(chunk_bytes)) {
        throw std::runtime_error("native buffer payload size changed");
    }
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count =
            write
                ? pwrite(descriptor, bytes.data() + offset, bytes.size() - offset, static_cast<off_t>(offset))
                : pread(descriptor, bytes.data() + offset, bytes.size() - offset, static_cast<off_t>(offset));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            throw std::runtime_error("native buffer payload transfer failed or was truncated");
        }
        offset += static_cast<std::size_t>(count);
    }
}
#endif
} // namespace

struct Graphics::Transport {
    std::mutex mutex;
    std::uint32_t sequence = 0;
    GraphicsDevice owned;
    std::set<std::uint32_t> fences;
    std::uint32_t last_fence = 0;
    std::map<std::uint32_t, GraphicsBuffer> buffers;
    std::uint32_t last_buffer = 0;
    int request = -1, response = -1;
    int upload = -1, download = -1;
    bool ready = false;

    ~Transport() {
#ifdef __linux__
        if (ready) {
            try {
                (void)exchange(4, 0);
            } catch (...) {
                // A dead service cannot acknowledge shutdown. Its bounded
                // lifetime owns cleanup; destructors must not throw or retry.
            }
        }
        for (const auto descriptor : {request, response, upload, download}) {
            if (descriptor >= 0) {
                (void)close(descriptor);
            }
        }
#endif
    }

    Packet exchange(std::uint32_t operation, std::uint32_t owner, std::uint64_t value = 0,
                    std::uint32_t request_flags = 0, std::uint32_t bytes = 0, std::uint32_t destination = 0,
                    std::uint64_t destination_offset = 0) {
#ifdef __linux__
        if (sequence >= 1024) {
            throw std::runtime_error("native graphics request budget exhausted");
        }
        Packet command;
        command.sequence = ++sequence;
        command.operation = operation;
        command.owner = owner;
        command.value = value;
        command.status = request_flags;
        command.payload = bytes;
        command.unused[0] = destination;
        command.reserved[0] = static_cast<std::uint32_t>(destination_offset);
        command.reserved[1] = static_cast<std::uint32_t>(destination_offset >> 32);
        transfer(request, command, true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        Packet reply;
        for (;;) {
            transfer(response, reply, false);
            if (reply.sequence == command.sequence) {
                break;
            }
            if (reply.sequence > command.sequence || std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error("native graphics reply deadline or sequence failed");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (reply.signature != magic || reply.operation != operation || reply.status || reply.payload ||
            reply.unused != std::array<std::uint32_t, 2>{} ||
            reply.reserved != std::array<std::uint32_t, 4>{}) {
            throw std::runtime_error(
                "native graphics service rejected the request or returned invalid fields");
        }
        if (operation != 6 && operation != 9 && operation != 10 && reply.value) {
            throw std::runtime_error("unexpected native graphics response value");
        }
        if (operation == 10) {
            if (!reply.owner || reply.owner > 128 || !reply.value || reply.api_version ||
                value > UINT64_MAX - reply.value) {
                throw std::runtime_error("invalid native buffer creation reply");
            }
        } else if (operation >= 11 && operation <= 14) {
            if (reply.owner != owner || reply.api_version) {
                throw std::runtime_error("invalid native buffer operation reply");
            }
        } else if (operation == 5) {
            if (!reply.owner || reply.owner > 128 || reply.api_version) {
                throw std::runtime_error("invalid native fence creation reply");
            }
        } else if (operation == 9) {
            if (reply.owner != owner || reply.api_version || (reply.value != 1 && reply.value != 2)) {
                throw std::runtime_error("invalid native device heap model reply");
            }
        } else if (operation >= 6 && operation <= 8) {
            if (reply.owner != owner || reply.api_version || reply.value == UINT64_MAX) {
                throw std::runtime_error("invalid native fence operation reply");
            }
        } else if (operation == 1 || operation == 3) {
            if (!reply.owner || reply.owner > 64 || reply.api_version < minimum_api ||
                (operation == 3 && reply.owner != owner)) {
                throw std::runtime_error("invalid native graphics device ownership reply");
            }
        } else if (reply.owner != owner || reply.api_version) {
            throw std::runtime_error("invalid native graphics release reply");
        }
        return reply;
#else
        (void)operation;
        (void)owner;
        (void)value;
        (void)request_flags;
        (void)bytes;
        (void)destination;
        (void)destination_offset;
        throw std::runtime_error("native graphics file transport is unsupported on this host");
#endif
    }

    void buffer_range(std::uint32_t id, std::uint64_t offset, std::size_t bytes) const {
        const auto found = buffers.find(id);
        if (found == buffers.end() || !bytes || bytes > chunk_bytes || offset > found->second.bytes ||
            bytes > found->second.bytes - offset) {
            throw std::runtime_error("native transfer exceeds its owned buffer range or chunk budget");
        }
    }
};

Graphics::Graphics(const std::filesystem::path &endpoint_root) : transport_(std::make_unique<Transport>()) {
#ifdef __linux__
    const auto directory = open(endpoint_root.c_str(), O_DIRECTORY | O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0) {
        throw std::runtime_error("cannot open explicit native graphics endpoints");
    }
    try {
        const auto endpoint = [&](const char *name, int access, std::size_t bytes = sizeof(Packet),
                                  bool optional = false) {
            const auto descriptor = openat(directory, name, access | O_CLOEXEC | O_NOFOLLOW);
            if (descriptor < 0 && optional && errno == ENOENT) {
                return -1;
            }
            if (descriptor < 0) {
                throw std::runtime_error("cannot open native graphics endpoint");
            }
            struct stat information {};
            if (fstat(descriptor, &information) || !S_ISREG(information.st_mode) ||
                information.st_nlink != 1 || information.st_size != static_cast<off_t>(bytes)) {
                (void)close(descriptor);
                throw std::runtime_error("invalid native graphics endpoint type or size");
            }
            return descriptor;
        };
        transport_->request = endpoint("request", O_RDWR);
        transport_->response = endpoint("response", O_RDONLY);
        transport_->upload = endpoint("upload", O_RDWR, chunk_bytes, true);
        transport_->download = endpoint("download", O_RDONLY, chunk_bytes, true);
        if ((transport_->upload < 0) != (transport_->download < 0)) {
            throw std::runtime_error("native buffer payload endpoints are incomplete");
        }
        if (flock(transport_->request, LOCK_EX | LOCK_NB)) {
            throw std::runtime_error("native graphics endpoints already have a client owner");
        }
        Packet initial;
        transfer(transport_->response, initial, false);
        const auto *bytes = reinterpret_cast<const std::byte *>(&initial);
        if (std::any_of(bytes, bytes + sizeof(initial), [](auto value) { return value != std::byte{0}; })) {
            throw std::runtime_error("native graphics requires a fresh service session");
        }
        transport_->ready = true;
        (void)close(directory);
    } catch (...) {
        (void)close(directory);
        throw;
    }
#else
    (void)endpoint_root;
    throw std::runtime_error("native graphics file transport is unsupported on this host");
#endif
}

Graphics::~Graphics() = default;

GraphicsDevice Graphics::create_device() {
    const std::lock_guard lock(transport_->mutex);
    if (transport_->owned.id) {
        throw std::runtime_error("native graphics already owns a device");
    }
    const auto reply = transport_->exchange(1, 0);
    transport_->owned = {reply.owner, reply.api_version};
    return transport_->owned;
}

GraphicsDevice Graphics::query_device(std::uint32_t id) {
    const std::lock_guard lock(transport_->mutex);
    if (!id || id != transport_->owned.id) {
        throw std::runtime_error("native graphics device is not owned by this session");
    }
    const auto reply = transport_->exchange(3, id);
    if (reply.api_version != transport_->owned.api_version) {
        throw std::runtime_error("native graphics device capability changed");
    }
    return {reply.owner, reply.api_version};
}

void Graphics::destroy_device(std::uint32_t id) {
    const std::lock_guard lock(transport_->mutex);
    if (!id || id != transport_->owned.id) {
        throw std::runtime_error("native graphics device is not owned by this session");
    }
    if (!transport_->fences.empty() || !transport_->buffers.empty()) {
        throw std::runtime_error("native graphics device still owns live fences or buffers");
    }
    (void)transport_->exchange(2, id);
    transport_->owned = {};
}

bool Graphics::unified_memory() {
    const std::lock_guard lock(transport_->mutex);
    if (!transport_->owned.id) {
        throw std::runtime_error("native heap properties require an owned device");
    }
    return transport_->exchange(9, transport_->owned.id).value == 1;
}

std::uint32_t Graphics::create_fence(std::uint64_t initial, bool shared) {
    const std::lock_guard lock(transport_->mutex);
    if (!transport_->owned.id || initial == UINT64_MAX || transport_->fences.size() >= 32) {
        throw std::runtime_error("native fence creation requires an owned device and bounded value/capacity");
    }
    const auto reply = transport_->exchange(5, transport_->owned.id, initial, shared ? 1u : 0u);
    if (reply.owner <= transport_->last_fence) {
        throw std::runtime_error("native fence service reused an ownership ID");
    }
    try {
        transport_->fences.insert(reply.owner);
    } catch (...) {
        (void)transport_->exchange(8, reply.owner);
        throw;
    }
    transport_->last_fence = reply.owner;
    return reply.owner;
}

std::uint64_t Graphics::completed_fence(std::uint32_t id) {
    const std::lock_guard lock(transport_->mutex);
    if (!transport_->fences.contains(id)) {
        throw std::runtime_error("native fence is not owned by this session");
    }
    return transport_->exchange(6, id).value;
}

void Graphics::signal_fence(std::uint32_t id, std::uint64_t value) {
    const std::lock_guard lock(transport_->mutex);
    if (!transport_->fences.contains(id) || value == UINT64_MAX) {
        throw std::runtime_error("native fence signal requires an owned fence and bounded value");
    }
    (void)transport_->exchange(7, id, value);
}

void Graphics::destroy_fence(std::uint32_t id) {
    const std::lock_guard lock(transport_->mutex);
    if (!transport_->fences.contains(id)) {
        throw std::runtime_error("native fence is not owned by this session");
    }
    (void)transport_->exchange(8, id);
    transport_->fences.erase(id);
}

GraphicsBuffer Graphics::create_buffer(std::uint64_t bytes) {
    const std::lock_guard lock(transport_->mutex);
    if (!transport_->owned.id || transport_->upload < 0 || !bytes || bytes > maximum_buffer_bytes ||
        transport_->buffers.size() >= 32) {
        throw std::runtime_error(
            "native buffer creation requires owned device, payload files and bounded size");
    }
    const auto reply = transport_->exchange(10, transport_->owned.id, bytes);
    if (reply.owner <= transport_->last_buffer) {
        throw std::runtime_error("native buffer service reused an ownership ID");
    }
    const GraphicsBuffer result{reply.owner, bytes, reply.value};
    try {
        transport_->buffers.emplace(reply.owner, result);
    } catch (...) {
        (void)transport_->exchange(11, reply.owner);
        throw;
    }
    transport_->last_buffer = reply.owner;
    return result;
}

void Graphics::destroy_buffer(std::uint32_t id) {
    const std::lock_guard lock(transport_->mutex);
    if (!transport_->buffers.contains(id)) {
        throw std::runtime_error("native buffer is not owned by this session");
    }
    (void)transport_->exchange(11, id);
    transport_->buffers.erase(id);
}

void Graphics::upload_buffer(std::uint32_t id, std::uint64_t offset, std::span<const std::byte> bytes) {
    const std::lock_guard lock(transport_->mutex);
    transport_->buffer_range(id, offset, bytes.size());
#ifdef __linux__
    // pwrite does not modify the source. Keep the mutable span confined to the
    // shared read/write transfer helper, never to the public upload interface.
    transfer_bytes(transport_->upload, {const_cast<std::byte *>(bytes.data()), bytes.size()}, true);
    (void)transport_->exchange(12, id, offset, 0, static_cast<std::uint32_t>(bytes.size()));
#else
    throw std::runtime_error("native buffer file transport is unsupported on this host");
#endif
}

void Graphics::download_buffer(std::uint32_t id, std::uint64_t offset, std::span<std::byte> bytes) {
    const std::lock_guard lock(transport_->mutex);
    transport_->buffer_range(id, offset, bytes.size());
#ifdef __linux__
    (void)transport_->exchange(13, id, offset, 0, static_cast<std::uint32_t>(bytes.size()));
    transfer_bytes(transport_->download, bytes, false);
#else
    throw std::runtime_error("native buffer file transport is unsupported on this host");
#endif
}

void Graphics::copy_buffer(std::uint32_t source, std::uint64_t source_offset, std::uint32_t destination,
                           std::uint64_t destination_offset, std::uint32_t bytes) {
    const std::lock_guard lock(transport_->mutex);
    transport_->buffer_range(source, source_offset, bytes);
    transport_->buffer_range(destination, destination_offset, bytes);
    if (source_offset % 4 || destination_offset % 4 || bytes % 4 ||
        (source == destination && source_offset < destination_offset + bytes &&
         destination_offset < source_offset + bytes)) {
        throw std::runtime_error("native buffer copy ranges are unaligned or overlap");
    }
    (void)transport_->exchange(14, source, source_offset, 0, bytes, destination, destination_offset);
}

} // namespace ambrosia::host
