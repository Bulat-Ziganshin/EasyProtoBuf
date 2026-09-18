#include "logical_paths.hpp"

#include <cstddef>

namespace easypb_schema {
namespace {

// Portable strict UTF-8 well-formedness: no overlong encodings,
// surrogate code points, truncated sequences, or values above U+10FFFF.
// No platform headers, so descriptor-only builds reuse it unchanged.
bool is_well_formed_utf8(const std::string& text)
{
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char a = static_cast<unsigned char>(text[i]);
        if (a <= 0x7Fu) {
            ++i;
            continue;
        }

        if (a >= 0xC2u && a <= 0xDFu) {
            if (i + 1 >= text.size()) return false;
            const unsigned char b = static_cast<unsigned char>(text[i + 1]);
            if (b < 0x80u || b > 0xBFu) return false;
            i += 2;
            continue;
        }

        if (a >= 0xE0u && a <= 0xEFu) {
            if (i + 2 >= text.size()) return false;
            const unsigned char b = static_cast<unsigned char>(text[i + 1]);
            const unsigned char c = static_cast<unsigned char>(text[i + 2]);
            if (c < 0x80u || c > 0xBFu) return false;
            if (a == 0xE0u) {
                if (b < 0xA0u || b > 0xBFu) return false;
            } else if (a == 0xEDu) {
                if (b < 0x80u || b > 0x9Fu) return false;
            } else if (b < 0x80u || b > 0xBFu) {
                return false;
            }
            i += 3;
            continue;
        }

        if (a >= 0xF0u && a <= 0xF4u) {
            if (i + 3 >= text.size()) return false;
            const unsigned char b = static_cast<unsigned char>(text[i + 1]);
            const unsigned char c = static_cast<unsigned char>(text[i + 2]);
            const unsigned char d = static_cast<unsigned char>(text[i + 3]);
            if (c < 0x80u || c > 0xBFu || d < 0x80u || d > 0xBFu) {
                return false;
            }
            if (a == 0xF0u) {
                if (b < 0x90u || b > 0xBFu) return false;
            } else if (a == 0xF4u) {
                if (b < 0x80u || b > 0x8Fu) return false;
            } else if (b < 0x80u || b > 0xBFu) {
                return false;
            }
            i += 4;
            continue;
        }

        return false;
    }
    return true;
}

} // namespace

bool validate_logical_path(const std::string& path, std::string& reason)
{
    if (path.empty()) {
        reason = "empty import path";
        return false;
    }
    if (!is_well_formed_utf8(path)) {
        reason = "import path must be valid UTF-8";
        return false;
    }
    // Absolute paths, drive prefixes, and any ':' are not logical names.
    // A ':' never appears in a valid import; rejecting it here also covers
    // Windows drive spellings such as "C:/x.proto" and "C:x.proto".
    for (std::size_t i = 0; i < path.size(); ++i) {
        const unsigned char c =
            static_cast<unsigned char>(path[i]);
        if (c == ':') {
            reason = "import path must be relative, not absolute or drive-qualified";
            return false;
        }
        if (c == '\\') {
            reason = "import path must use '/' separators, not backslashes";
            return false;
        }
        if (c == '"' || c == '\'') {
            reason = "import path must not contain quotes";
            return false;
        }
        if (c < 0x20u || c == 0x7Fu) {
            reason = "import path must not contain control characters";
            return false;
        }
    }
    if (path[0] == '/') {
        reason = "import path must be relative, not absolute";
        return false;
    }

    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t end = path.find('/', start);
        if (end == std::string::npos) end = path.size();
        const std::size_t length = end - start;
        if (length == 0) {
            reason = "import path must not contain empty components";
            return false;
        }
        if (length == 1 && path[start] == '.') {
            reason = "import path must not contain '.' components";
            return false;
        }
        if (length == 2 && path[start] == '.' && path[start + 1] == '.') {
            reason = "import path must not contain '..' components";
            return false;
        }
        if (end == path.size()) break;
        start = end + 1;
    }
    return true;
}

bool is_valid_logical_path(const std::string& path)
{
    std::string reason;
    return validate_logical_path(path, reason);
}

} // namespace easypb_schema
