// Host services used by the portable PE core. No guest Windows types here.
#pragma once
#include <cstddef>
#include <filesystem>

namespace ambrosia::host {
enum class Access { none, read, read_write, read_execute };

class Mapping {
public:
    explicit Mapping(std::size_t bytes);
    ~Mapping();
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    Mapping(Mapping&& other) noexcept;
    Mapping& operator=(Mapping&& other) noexcept;
    void protect(std::size_t offset, std::size_t bytes, Access access);
    void flush_instructions();
    std::byte* data() const { return data_; }
    std::size_t size() const { return size_; }
    static std::size_t page_size();
private:
    void release() noexcept;
    std::byte* data_ = nullptr;
    std::size_t size_ = 0;
};

struct Paths {
    std::filesystem::path image;
    std::filesystem::path package;
    std::filesystem::path work;
};
// Resolve host paths without changing cwd or creating directories. Explicit
// work roots override host cache conventions; inputs must already exist.
Paths resolve_paths(const std::filesystem::path& image,
                    const std::filesystem::path& package = {},
                    const std::filesystem::path& work = {});
} // namespace ambrosia::host
