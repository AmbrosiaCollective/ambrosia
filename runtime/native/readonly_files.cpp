#include "readonly_files.h"
#include "text_conversion.h"
#include <algorithm>
#include <stdexcept>
#include <string>

namespace ambrosia::services {
namespace {
constexpr std::size_t path_limit = 131068;

std::uint32_t guest_error(host::FileLookupError error) {
    switch (error) {
    case host::FileLookupError::none:
        return 0;
    case host::FileLookupError::missing:
        return 2;
    case host::FileLookupError::missing_directory:
        return 3;
    case host::FileLookupError::denied:
        return 5;
    case host::FileLookupError::invalid:
        return 123;
    case host::FileLookupError::too_long:
        return 206;
    case host::FileLookupError::exhausted:
        return 8;
    case host::FileLookupError::unsupported:
        return 50;
    case host::FileLookupError::failure:
        return 31;
    }
    return 31;
}

ResolvedFilePath failure(std::uint32_t error) {
    return {nullptr, {}, false, error};
}

std::uint32_t components(std::string_view path, std::vector<std::string> &output) {
    std::size_t begin = 0;
    while (begin <= path.size()) {
        const auto slash = path.find('/', begin);
        const auto end = slash == std::string_view::npos ? path.size() : slash;
        const auto name = path.substr(begin, end - begin);
        if (name == "..") {
            if (output.empty()) {
                return 5;
            }
            output.pop_back();
        } else if (!name.empty() && name != ".") {
            if (name.find_first_of(":<>\"|?*") != std::string_view::npos || name.back() == ' ' ||
                name.back() == '.') {
                return 123;
            }
            output.emplace_back(name);
            if (output.size() > 1024) {
                return 206;
            }
        }
        if (slash == std::string_view::npos) {
            break;
        }
        begin = slash + 1;
    }
    return 0;
}

bool volume_hint(std::string_view name) {
    if (name.empty() || name.size() > 16) {
        return false;
    }
    const auto letter = [](unsigned char unit) {
        return (unit >= 'a' && unit <= 'z') || (unit >= 'A' && unit <= 'Z');
    };
    if (!letter(static_cast<unsigned char>(name.front()))) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [&](unsigned char unit) {
        return letter(unit) || (unit >= '0' && unit <= '9') || unit == '_';
    });
}

std::filesystem::path native_path(std::string_view path) {
    return std::filesystem::path(std::u8string(path.begin(), path.end()));
}

std::string portable_path(const std::filesystem::path &path) {
    const auto utf8 = path.generic_u8string();
    return std::string(utf8.begin(), utf8.end());
}
} // namespace

struct ReadonlyFiles::Roots {
    struct Root {
        std::filesystem::path path;
        std::shared_ptr<const host::ReadonlyDirectory> directory;
    };

    std::vector<Root> entries;
};

void ReadonlyFiles::bind(const std::filesystem::path &package, const std::filesystem::path &image) {
    auto roots = std::make_shared<Roots>();
    const auto add = [&](const std::filesystem::path &path) {
        const auto canonical = std::filesystem::canonical(path);
        for (const auto &entry : roots->entries) {
            if (entry.path == canonical) {
                return;
            }
        }
        roots->entries.push_back({canonical, std::make_shared<host::ReadonlyDirectory>(canonical)});
    };
    add(package);
    add(image.parent_path());
    const std::lock_guard lock(mutex_);
    if (roots_) {
        throw std::runtime_error("read-only guest roots are already bound");
    }
    roots_ = std::move(roots);
}

void ReadonlyFiles::bind_additional_native_root(const std::filesystem::path &path) {
    const auto canonical = std::filesystem::canonical(path);
    auto directory = std::make_shared<const host::ReadonlyDirectory>(canonical);
    const std::lock_guard lock(mutex_);
    if (!roots_ || roots_->entries.size() >= 8) {
        throw std::runtime_error("additional native read root requires bound assets and available capacity");
    }
    for (const auto &entry : roots_->entries) {
        const auto nested = [](const auto &child, const auto &parent) {
            const auto relative = child.lexically_relative(parent);
            return !relative.empty() && !relative.is_absolute() &&
                   *relative.begin() != std::filesystem::path("..");
        };
        if (nested(canonical, entry.path) || nested(entry.path, canonical)) {
            throw std::runtime_error("additional native read root must be separate from existing roots");
        }
    }
    auto roots = std::make_shared<Roots>(*roots_);
    roots->entries.push_back({canonical, std::move(directory)});
    roots_ = std::move(roots);
}

ResolvedFilePath ReadonlyFiles::resolve(std::string_view requested) const {
    std::shared_ptr<const Roots> roots;
    {
        const std::lock_guard lock(mutex_);
        roots = roots_;
    }
    if (!roots) {
        return failure(1);
    }
    if (requested.empty() || requested.find('\0') != std::string_view::npos) {
        return failure(123);
    }
    if (requested.size() > path_limit) {
        return failure(206);
    }
    const auto wide = unicode::utf8_to_utf16(requested, true);
    if (!wide) {
        return failure(123);
    }
    if (wide->size() > 32767) {
        return failure(206);
    }
    const bool windows_rooted = requested.front() == '\\';
    std::string path(requested);
    std::replace(path.begin(), path.end(), '\\', '/');
    // Split the NT prefix literal to avoid the obsolete trigraph spelling.
    if (path.starts_with("//?/") || path.starts_with("/?"
                                                     "?/")) {
        path.erase(0, 4);
    }
    if (path.empty()) {
        return failure(123);
    }
    const bool unc = path.size() >= 4 && (path[0] == 'U' || path[0] == 'u') &&
                     (path[1] == 'N' || path[1] == 'n') && (path[2] == 'C' || path[2] == 'c') &&
                     path[3] == '/';
    if (path.starts_with("//") || unc) {
        return failure(5);
    }
    ResolvedFilePath result;
    result.directory_only =
        path.back() == '/' || path == "." || path == ".." || path.ends_with("/.") || path.ends_with("/..");
    const auto native = native_path(path);
    if (native.is_absolute()) {
        // Lexical routing chooses only a configured root. Descriptor-relative
        // lookup independently forbids symlink/mount escapes after routing.
        const auto normalized = native.lexically_normal();
        for (const auto &root : roots->entries) {
            const auto relative = normalized.lexically_relative(root.path);
            if (relative.empty() || relative.is_absolute()) {
                continue;
            }
            const auto text = portable_path(relative);
            if (text == ".." || text.starts_with("../")) {
                continue;
            }
            const auto error = components(text, result.components);
            if (error) {
                return failure(error);
            }
            result.directory = root.directory;
            return result;
        }
        if (!windows_rooted) {
            return failure(5);
        }
    }
    const auto colon = path.find(':');
    if (colon != std::string::npos) {
        if (!volume_hint(std::string_view(path).substr(0, colon))) {
            return failure(123);
        }
        path.erase(0, colon + 1);
        if (!path.empty() && path.front() != '/') {
            return failure(123); // Per-drive working directories are not modeled.
        }
        if (path.starts_with("//")) {
            return failure(5); // No URI/network namespace is mounted.
        }
    }
    const auto first_name = path.find_first_not_of('/');
    if (first_name == std::string::npos) {
        path.clear();
    } else {
        path.erase(0, first_name);
    }
    const auto error = components(path, result.components);
    if (error) {
        return failure(error);
    }
    result.directory = roots->entries.front().directory;
    return result;
}

FileAttributes ReadonlyFiles::attributes(std::string_view path) const {
    const auto resolved = resolve(path);
    if (resolved.error) {
        return {0xffffffffu, resolved.error};
    }
    const auto found = resolved.directory->inspect(resolved.components);
    if (found.error != host::FileLookupError::none) {
        return {0xffffffffu, guest_error(found.error)};
    }
    if (resolved.directory_only && !found.metadata.directory) {
        return {0xffffffffu, 3};
    }
    // These attributes describe this read-only virtual namespace. Do not
    // invent host archive/hidden/system flags from filename conventions.
    return {1u | (found.metadata.directory ? 0x10u : 0u), 0};
}

ReadonlyFileOpen ReadonlyFiles::open(std::string_view path, bool read_data, std::uint32_t share,
                                     bool unbuffered) const {
    if (share & ~7u) {
        return {nullptr, 87};
    }
    const auto resolved = resolve(path);
    if (resolved.error) {
        return {nullptr, resolved.error};
    }
    if (resolved.directory_only) {
        return {nullptr, 5};
    }
    auto result = resolved.directory->open(resolved.components, unbuffered);
    if (result.error != host::FileLookupError::none) {
        return {nullptr, guest_error(result.error)};
    }
    const auto found = result.file->inspect();
    if (found.error != host::FileLookupError::none) {
        return {nullptr, guest_error(found.error)};
    }
    const std::lock_guard lock(mutex_);
    std::erase_if(open_files_, [](const auto &entry) { return entry.file.expired(); });
    if (open_files_.size() >= 32) {
        return {nullptr, 8};
    }
    for (const auto &entry : open_files_) {
        if (entry.device == found.metadata.device && entry.identity == found.metadata.identity && read_data &&
            entry.read_data && (!(entry.share & 1) || !(share & 1))) {
            return {nullptr, 32}; // Symmetric read sharing, also for hard-link aliases.
        }
    }
    std::shared_ptr<host::ReadonlyFile> retained(std::move(result.file));
    open_files_.push_back({retained, found.metadata.device, found.metadata.identity, read_data, share});
    return {std::move(retained), 0};
}
} // namespace ambrosia::services
