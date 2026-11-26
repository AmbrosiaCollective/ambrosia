#include "package_identity.h"
#include <charconv>
#include <expat.h>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ambrosia::services {
namespace {
static_assert(std::is_same_v<XML_Char, char>);

std::string_view local_name(const char *name) {
    const std::string_view value(name);
    const auto separator = value.rfind('|');
    return separator == std::string_view::npos ? value : value.substr(separator + 1);
}

bool valid_name(std::string_view name) {
    if (name.empty() || name.size() > package_identifier_limit) {
        return false;
    }
    for (const unsigned char unit : name) {
        if (!((unit >= 'A' && unit <= 'Z') || (unit >= 'a' && unit <= 'z') || (unit >= '0' && unit <= '9') ||
              unit == '.' || unit == '-')) {
            return false;
        }
    }
    return true;
}

std::optional<std::array<std::uint16_t, 4>> parse_version(std::string_view text) {
    std::array<std::uint16_t, 4> result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        const auto separator = text.find('.');
        const auto component = text.substr(0, separator);
        if (component.empty()) {
            return std::nullopt;
        }
        for (const unsigned char unit : component) {
            if (unit < '0' || unit > '9') {
                return std::nullopt;
            }
        }
        unsigned value = 0;
        const auto parsed = std::from_chars(component.data(), component.data() + component.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != component.data() + component.size() || value > 65535) {
            return std::nullopt;
        }
        result[index] = static_cast<std::uint16_t>(value);
        if (index + 1 == result.size()) {
            if (separator != std::string_view::npos) {
                return std::nullopt;
            }
        } else {
            if (separator == std::string_view::npos) {
                return std::nullopt;
            }
            text.remove_prefix(separator + 1);
        }
    }
    return result;
}

struct ParseState {
    XML_Parser parser;
    std::string_view expected_root;
    std::optional<PackageIdentity> identity;
    std::size_t depth = 0;
    std::size_t elements = 0;
    bool root_seen = false;
    bool failed = false;
    bool exhausted = false;

    ParseState(XML_Parser value, std::string_view root) : parser(value), expected_root(root) {}

    void reject() noexcept {
        failed = true;
        (void)XML_StopParser(parser, XML_FALSE);
    }

    void start(const char *name, const char **attributes) {
        unsigned attribute_count = 0;
        for (auto entry = attributes; *entry; entry += 2) {
            if (++attribute_count > 64 || std::string_view(entry[1]).size() > 1024) {
                reject();
                return;
            }
        }
        if (++depth > 32 || ++elements > 2048) {
            reject();
            return;
        }
        if (depth == 1) {
            root_seen = local_name(name) == expected_root;
            if (!root_seen) {
                reject();
            }
            return;
        }
        if (depth != 2 || local_name(name) != "Identity") {
            return;
        }
        if (identity) {
            reject();
            return;
        }
        PackageIdentity parsed;
        std::string_view version;
        unsigned count = 0;
        for (auto entry = attributes; *entry; entry += 2) {
            if (++count > 64) {
                reject();
                return;
            }
            const std::string_view key(*entry);
            const std::string_view value(entry[1]);
            if (value.size() > 1024) {
                reject();
                return;
            }
            if (key == "Name") {
                parsed.name = value;
            } else if (key == "Publisher") {
                parsed.publisher = value;
            } else if (key == "Version") {
                version = value;
            }
        }
        const auto parts = parse_version(version);
        if (!valid_name(parsed.name) || parsed.publisher.empty() || parsed.publisher.size() > 256 || !parts) {
            reject();
            return;
        }
        parsed.version = *parts;
        identity = std::move(parsed);
    }
};

void XMLCALL start_element(void *context, const char *name, const char **attributes) noexcept {
    auto &state = *static_cast<ParseState *>(context);
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
    auto &state = *static_cast<ParseState *>(context);
    if (!state.depth) {
        state.reject();
        return;
    }
    --state.depth;
}

void XMLCALL reject_doctype(void *context, const char *, const char *, const char *, int) noexcept {
    static_cast<ParseState *>(context)->reject();
}

int XMLCALL reject_external_entity(XML_Parser, const char *, const char *, const char *,
                                   const char *) noexcept {
    return XML_STATUS_ERROR;
}
} // namespace

PackageIdentity parse_package_identity(std::span<const std::byte> input, PackageDescriptor kind) {
    if (input.empty() || input.size() > package_descriptor_limit) {
        throw std::runtime_error("package descriptor size is invalid");
    }
    bool compatible = false;
    const auto *features = XML_GetFeatureList();
    if (features) {
        for (std::size_t index = 0; index < 64 && features[index].feature != XML_FEATURE_END; ++index) {
            if (features[index].feature == XML_FEATURE_SIZEOF_XML_CHAR) {
                compatible = features[index].value == static_cast<long>(sizeof(XML_Char));
            }
        }
    }
    if (!compatible) {
        throw std::runtime_error("package descriptors require the UTF8 Expat character ABI");
    }
    using Parser = std::unique_ptr<std::remove_pointer_t<XML_Parser>, decltype(&XML_ParserFree)>;
    Parser parser(XML_ParserCreateNS(nullptr, '|'), &XML_ParserFree);
    if (!parser) {
        throw std::bad_alloc();
    }
    ParseState state{parser.get(), kind == PackageDescriptor::game_config ? "Game" : "Package"};
    XML_SetUserData(parser.get(), &state);
    XML_SetElementHandler(parser.get(), &start_element, &end_element);
    XML_SetStartDoctypeDeclHandler(parser.get(), &reject_doctype);
    XML_SetExternalEntityRefHandler(parser.get(), &reject_external_entity);
    (void)XML_SetParamEntityParsing(parser.get(), XML_PARAM_ENTITY_PARSING_NEVER);
    const auto status = XML_Parse(parser.get(), reinterpret_cast<const char *>(input.data()),
                                  static_cast<int>(input.size()), XML_TRUE);
    if (state.exhausted) {
        throw std::bad_alloc();
    }
    if (status != XML_STATUS_OK || state.failed || !state.root_seen || state.depth || !state.identity) {
        throw std::runtime_error("package descriptor has no valid bounded identity");
    }
    return std::move(*state.identity);
}
} // namespace ambrosia::services
