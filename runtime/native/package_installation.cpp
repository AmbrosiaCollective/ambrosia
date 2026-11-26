#include "package_installation.h"
#include "text_conversion.h"
#include <charconv>
#include <expat.h>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace ambrosia::services {
namespace {
std::string folded_path(std::string_view path) {
    std::string result(path);
    for (auto &unit : result) {
        if (unit >= 'A' && unit <= 'Z') {
            unit = static_cast<char>(unit + ('a' - 'A'));
        }
    }
    return result;
}

bool valid_relative_path(std::string_view path) {
    if (path.empty() || path.size() > 1024 || !unicode::utf8_to_utf16(path, true)) {
        return false;
    }
    for (const unsigned char unit : path) {
        if (unit < 32 || unit == 127 || std::string_view("\\:<>\"|?*").find(unit) != std::string_view::npos) {
            return false;
        }
    }
    unsigned components = 0;
    while (true) {
        const auto slash = path.find('/');
        const auto name = path.substr(0, slash);
        if (++components > 64 || name.empty() || name == "." || name == ".." || name.back() == '.' ||
            name.back() == ' ') {
            return false;
        }
        if (slash == std::string_view::npos) {
            return true;
        }
        path.remove_prefix(slash + 1);
    }
}

struct InventoryParser {
    XML_Parser parser;
    std::vector<PackageFileRequirement> files;
    std::set<std::string> names;
    unsigned depth = 0;
    bool inventory_seen = false;
    bool inside_inventory = false;
    bool failed = false;
    bool exhausted = false;

    void reject() noexcept {
        failed = true;
        (void)XML_StopParser(parser, XML_FALSE);
    }

    void start(const char *name, const char **attributes) {
        const std::string_view element(name);
        ++depth;
        if (depth == 1) {
            if (element != "Package" || *attributes) {
                reject();
            }
        } else if (depth == 2 && element == "Identity") {
            // The identity parser already validated this exact document.
        } else if (depth == 2 && element == "Inventory" && !inventory_seen && !*attributes) {
            inventory_seen = inside_inventory = true;
        } else if (depth == 3 && inside_inventory && element == "File") {
            std::string_view path;
            std::string_view size;
            unsigned count = 0;
            for (auto entry = attributes; *entry; entry += 2) {
                ++count;
                if (std::string_view(entry[0]) == "Path") {
                    path = entry[1];
                } else if (std::string_view(entry[0]) == "Size") {
                    size = entry[1];
                } else {
                    reject();
                    return;
                }
            }
            if (size.empty()) {
                reject();
                return;
            }
            std::uint64_t bytes = 0;
            const auto parsed = std::from_chars(size.data(), size.data() + size.size(), bytes);
            if (count != 2 || size.empty() || size.front() < '0' || size.front() > '9' ||
                parsed.ec != std::errc{} || parsed.ptr != size.data() + size.size() ||
                bytes > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
                !valid_relative_path(path) || files.size() >= 1024) {
                reject();
                return;
            }
            if (!names.insert(folded_path(path)).second) {
                reject();
                return;
            }
            files.push_back({std::string(path), bytes});
        } else {
            reject();
        }
    }
};

void XMLCALL start_element(void *context, const char *name, const char **attributes) noexcept {
    auto &state = *static_cast<InventoryParser *>(context);
    try {
        state.start(name, attributes);
    } catch (const std::bad_alloc &) {
        state.exhausted = true;
        state.reject();
    } catch (...) {
        state.reject();
    }
}

void XMLCALL end_element(void *context, const char *) noexcept {
    auto &state = *static_cast<InventoryParser *>(context);
    if (state.depth == 2) {
        state.inside_inventory = false;
    }
    --state.depth;
}

void XMLCALL characters(void *context, const char *text, int length) noexcept {
    for (int index = 0; index < length; ++index) {
        const char unit = text[index];
        if (unit != ' ' && unit != '\t' && unit != '\r' && unit != '\n') {
            static_cast<InventoryParser *>(context)->reject();
            return;
        }
    }
}
} // namespace

PackageInventory parse_package_inventory(std::span<const std::byte> input) {
    auto identity = parse_package_identity(input, PackageDescriptor::appx_manifest);
    const auto parser = XML_ParserCreate(nullptr);
    if (!parser) {
        throw std::bad_alloc();
    }
    const std::unique_ptr<std::remove_pointer_t<XML_Parser>, decltype(&XML_ParserFree)> owner(parser,
                                                                                              XML_ParserFree);
    InventoryParser state{parser, {}, {}};
    XML_SetUserData(parser, &state);
    XML_SetElementHandler(parser, start_element, end_element);
    XML_SetCharacterDataHandler(parser, characters);
    // The first, bounded identity pass rejects every DTD before this second pass.
    const auto result = XML_Parse(parser, reinterpret_cast<const char *>(input.data()),
                                  static_cast<int>(input.size()), XML_TRUE);
    if (state.exhausted) {
        throw std::bad_alloc();
    }
    if (result != XML_STATUS_OK || state.failed || !state.inventory_seen || state.files.empty()) {
        throw std::runtime_error("package inventory is invalid or exceeds its bounds");
    }
    return {std::move(identity), std::move(state.files)};
}

PackageInstallation::PackageInstallation(const PackageInventory &inventory, const ReadonlyFiles &files) {
    if (inventory.files.empty() || inventory.files.size() > 1024) {
        throw std::runtime_error("package inventory has an invalid file count");
    }
    std::set<std::string> names;
    for (const auto &entry : inventory.files) {
        if (!valid_relative_path(entry.path) || !names.insert(folded_path(entry.path)).second ||
            entry.bytes >
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) - total_bytes_) {
            throw std::runtime_error("package inventory has invalid paths or byte totals");
        }
        auto path = files.resolve(entry.path);
        if (path.error || path.directory_only || !path.directory) {
            throw std::runtime_error("package inventory cannot resolve a confined file");
        }
        entries_.push_back({std::move(path), entry.bytes});
        total_bytes_ += entry.bytes;
    }
}

InstallationReadiness PackageInstallation::inspect() const {
    InstallationReadiness result{total_bytes_, 0, static_cast<std::uint32_t>(entries_.size()), 0};
    for (const auto &entry : entries_) {
        const auto found = entry.path.directory->inspect(entry.path.components);
        if (found.error == host::FileLookupError::missing ||
            found.error == host::FileLookupError::missing_directory) {
            continue;
        }
        if (found.error != host::FileLookupError::none) {
            throw std::runtime_error("cannot inspect a confined package inventory file");
        }
        if (!found.metadata.directory && found.metadata.bytes == entry.bytes) {
            ++result.ready_files;
            result.installed_bytes += entry.bytes;
        }
    }
    return result;
}
} // namespace ambrosia::services
