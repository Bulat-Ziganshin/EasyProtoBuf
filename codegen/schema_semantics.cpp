#include "schema_semantics.hpp"
#include "schema_semantics_internal.hpp"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <utility>

namespace easypb_schema {

namespace {

// Build "parent.leaf" with ONE allocation. The chained form it replaces built
// two intermediates per name - `parent + "."` copied the parent, then the
// second '+' copied that again together with the leaf - and every declaration
// of a file is named this way, so the index phase paid it once per field, enum
// value, oneof and nested type.
std::string joined_name(const std::string& parent, const str_view& leaf)
{
    std::string result;
    result.reserve(parent.size() + 1 + leaf.size());
    result.append(parent);
    result.push_back('.');
    // W-35: an empty view may carry a null pointer, and append(nullptr, 0)
    // is formally undefined, so the empty leaf contributes only the dot.
    if (!leaf.empty()) result.append(leaf.data(), leaf.size());
    return result;
}

std::string format_path(const DescriptorPath& path)
{
    std::ostringstream output;
    output << "[";
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (i != 0) output << ", ";
        output << path[i];
    }
    output << "]";
    return output.str();
}

} // namespace

int builtin_type(const std::string& name)
{
    if (name == "double") return FieldDescriptorProto::TYPE_DOUBLE;
    if (name == "float") return FieldDescriptorProto::TYPE_FLOAT;
    if (name == "int64") return FieldDescriptorProto::TYPE_INT64;
    if (name == "uint64") return FieldDescriptorProto::TYPE_UINT64;
    if (name == "int32") return FieldDescriptorProto::TYPE_INT32;
    if (name == "fixed64") return FieldDescriptorProto::TYPE_FIXED64;
    if (name == "fixed32") return FieldDescriptorProto::TYPE_FIXED32;
    if (name == "bool") return FieldDescriptorProto::TYPE_BOOL;
    if (name == "string") return FieldDescriptorProto::TYPE_STRING;
    if (name == "bytes") return FieldDescriptorProto::TYPE_BYTES;
    if (name == "uint32") return FieldDescriptorProto::TYPE_UINT32;
    if (name == "sfixed32") return FieldDescriptorProto::TYPE_SFIXED32;
    if (name == "sfixed64") return FieldDescriptorProto::TYPE_SFIXED64;
    if (name == "sint32") return FieldDescriptorProto::TYPE_SINT32;
    if (name == "sint64") return FieldDescriptorProto::TYPE_SINT64;
    return 0;
}

bool packable_type(int type)
{
    return type != FieldDescriptorProto::TYPE_STRING &&
           type != FieldDescriptorProto::TYPE_BYTES &&
           type != FieldDescriptorProto::TYPE_MESSAGE &&
           type != FieldDescriptorProto::TYPE_GROUP && type != 0;
}

bool valid_map_key_type(int type)
{
    return type == FieldDescriptorProto::TYPE_INT32 ||
           type == FieldDescriptorProto::TYPE_INT64 ||
           type == FieldDescriptorProto::TYPE_UINT32 ||
           type == FieldDescriptorProto::TYPE_UINT64 ||
           type == FieldDescriptorProto::TYPE_SINT32 ||
           type == FieldDescriptorProto::TYPE_SINT64 ||
           type == FieldDescriptorProto::TYPE_FIXED32 ||
           type == FieldDescriptorProto::TYPE_FIXED64 ||
           type == FieldDescriptorProto::TYPE_SFIXED32 ||
           type == FieldDescriptorProto::TYPE_SFIXED64 ||
           type == FieldDescriptorProto::TYPE_BOOL ||
           type == FieldDescriptorProto::TYPE_STRING;
}

bool parse_unsigned_integer(const std::string& text, std::uint64_t& result)
{
    if (text.empty()) return false;
    unsigned base = 10;
    std::size_t position = 0;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        position = 2;
    } else if (text.size() > 1 && text[0] == '0') {
        base = 8;
        position = 1;
    }

    result = 0;
    if (position == text.size()) return true;
    for (; position < text.size(); ++position) {
        const char c = text[position];
        unsigned digit = 0;
        if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A' + 10);
        else return false;
        if (digit >= base) return false;
        if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / base) return false;
        result = result * base + digit;
    }
    return true;
}

bool parse_signed_32(const std::string& text, std::int32_t& result)
{
    if (text.empty()) return false;
    bool negative = false;
    std::size_t start = 0;
    if (text[0] == '+' || text[0] == '-') {
        negative = text[0] == '-';
        start = 1;
    }
    if (start == text.size()) return false;
    std::uint64_t magnitude = 0;
    if (!parse_unsigned_integer(text.substr(start), magnitude)) return false;
    const std::uint64_t limit = negative ? 2147483648ull : 2147483647ull;
    if (magnitude > limit) return false;
    if (negative && magnitude == 2147483648ull) result = std::numeric_limits<std::int32_t>::min();
    else result = negative ? -static_cast<std::int32_t>(magnitude)
                           : static_cast<std::int32_t>(magnitude);
    return true;
}

namespace {

bool parse_signed_64(const std::string& text, std::int64_t& result)
{
    if (text.empty()) return false;
    bool negative = false;
    std::size_t start = 0;
    if (text[0] == '+' || text[0] == '-') {
        negative = text[0] == '-';
        start = 1;
    }
    if (start == text.size()) return false;
    std::uint64_t magnitude = 0;
    if (!parse_unsigned_integer(text.substr(start), magnitude)) return false;
    const std::uint64_t negative_limit =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1u;
    const std::uint64_t limit =
        negative ? negative_limit
                 : static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (magnitude > limit) return false;
    if (negative && magnitude == negative_limit) result = std::numeric_limits<std::int64_t>::min();
    else result = negative ? -static_cast<std::int64_t>(magnitude)
                           : static_cast<std::int64_t>(magnitude);
    return true;
}

bool parse_unsigned_with_optional_plus(const std::string& text, std::uint64_t maximum)
{
    std::size_t start = 0;
    if (!text.empty() && text[0] == '+') start = 1;
    if (start == text.size() || (!text.empty() && text[0] == '-')) return false;
    std::uint64_t value = 0;
    return parse_unsigned_integer(text.substr(start), value) && value <= maximum;
}

} // namespace

namespace {

// protobuf's SimpleFtoa/SimpleDtoa keep at least two exponent digits but drop
// redundant leading zeros (older MSVCRT versions emit e-008). The formatting
// half is shared by both input paths after W-04.
std::string normalize_float_exponent(std::string text)
{
    const std::size_t marker = text.find_first_of("eE");
    if (marker == std::string::npos) return text;

    std::size_t first_digit = marker + 1;
    if (first_digit < text.size() &&
        (text[first_digit] == '+' || text[first_digit] == '-')) {
        ++first_digit;
    }

    while (first_digit + 2 < text.size() && text[first_digit] == '0') {
        text.erase(first_digit, 1);
    }
    return text;
}

std::string format_float(double value, int precision)
{
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(precision) << value;
    return normalize_float_exponent(output.str());
}

// ASCII whitespace, spelled out: `std::isspace` is locale-dependent, and a
// default value is bytes, not a locale's text.
bool is_ascii_whitespace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\v' || c == '\f';
}

char ascii_lower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string ascii_lowercase(const std::string& text)
{
    std::string lowered(text);
    for (std::size_t i = 0; i < lowered.size(); ++i) {
        lowered[i] = ascii_lower(lowered[i]);
    }
    return lowered;
}

// CR-02: the reference compiler accepts `INF`, `Infinity` and `NAN` because
// `io::NoLocaleStrtod` is case-insensitive, and it accepts the C99
// `nan(payload)` shape. The canonicalizer spells every one of them `inf`,
// `-inf` or `nan`; the payload never reaches generated C++.
bool is_infinity_spelling(const std::string& body)
{
    const std::string lowered = ascii_lowercase(body);
    return lowered == "inf" || lowered == "infinity";
}

bool is_quiet_nan_spelling(const std::string& body)
{
    const std::string lowered = ascii_lowercase(body);
    if (lowered == "nan") return true;
    return lowered.size() >= 5 && lowered.compare(0, 4, "nan(") == 0 &&
           lowered[lowered.size() - 1] == ')';
}

// The decimal spelling of a uint64 without std::to_string, which the legacy
// profile (GCC 4.7) does not have.
std::string u64_to_decimal(std::uint64_t value)
{
    char buffer[20];
    char* end = buffer + sizeof(buffer);
    char* begin = end;
    do {
        *--begin = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    return std::string(begin, end);
}

// The one formatting body: the text overload, the value overload and the
// parser's INTEGER-token branch all reach it.
std::string format_canonical_float(double value, bool single_precision)
{
    // W-04: overflow is not a refusal. `strtod` returns +-infinity and sets
    // ERANGE; protoc stores exactly `inf` / `-inf` and keeps going.
    if (std::isinf(value)) return std::signbit(value) ? "-inf" : "inf";
    if (std::isnan(value)) return "nan";

    if (single_precision) {
        const float narrowed = static_cast<float>(value);
        std::string formatted =
            format_float(narrowed, std::numeric_limits<float>::digits10);
        char* parsed_end = 0;
        const float round_trip =
            static_cast<float>(std::strtod(formatted.c_str(), &parsed_end));
        if (parsed_end == formatted.c_str() || *parsed_end != '\0' ||
            round_trip != narrowed) {
            formatted =
                format_float(narrowed, std::numeric_limits<float>::digits10 + 3);
        }
        return formatted;
    }

    std::string formatted =
        format_float(value, std::numeric_limits<double>::digits10);
    char* parsed_end = 0;
    const double round_trip = std::strtod(formatted.c_str(), &parsed_end);
    if (parsed_end == formatted.c_str() || *parsed_end != '\0' ||
        round_trip != value) {
        formatted =
            format_float(value, std::numeric_limits<double>::digits10 + 2);
    }
    return formatted;
}

} // namespace

bool can_normalize_default_text(const std::string& text)
{
    if (text.empty()) return false;
    return text[0] != '+' && !is_ascii_whitespace(text[0]);
}

bool canonical_float_default(const std::string& text, bool single_precision,
                             std::string& result)
{
    if (text.empty()) return false;

    std::size_t start = 0;
    if (text[0] == '+' || text[0] == '-') start = 1;
    const std::string body = text.substr(start);
    if (is_infinity_spelling(body)) {
        result = text[0] == '-' ? "-inf" : "inf";
        return true;
    }
    if (is_quiet_nan_spelling(body)) {
        result = "nan";
        return true;
    }

    char* end = 0;
    errno = 0;
    const double parsed = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0') return false;
    return canonical_float_default(parsed, single_precision, result);
}

bool canonical_float_default(double value, bool single_precision,
                             std::string& result)
{
    result = format_canonical_float(value, single_precision);
    return true;
}

bool is_canonical_float_default(const std::string& text,
                                bool single_precision)
{
    std::string canonical;
    return canonical_float_default(text, single_precision, canonical) &&
           canonical == text;
}

bool parse_integral_default_literal(const std::string& text, bool& negative,
                                    std::uint64_t& magnitude)
{
    negative = false;
    std::size_t start = 0;
    if (!text.empty() && text[0] == '-') {
        negative = true;
        start = 1;
    }
    if (start == text.size()) return false;
    return parse_unsigned_integer(text.substr(start), magnitude);
}

bool canonical_integral_default(const std::string& text, std::string& result)
{
    bool negative = false;
    std::uint64_t magnitude = 0;
    if (!parse_integral_default_literal(text, negative, magnitude)) {
        return false;
    }
    std::string canonical;
    if (negative && magnitude != 0) canonical.push_back('-');
    canonical += u64_to_decimal(magnitude);
    result.swap(canonical);
    return true;
}

bool integer_token_as_double(const std::string& text, double& result)
{
    // A leading sign is part of the stored token, not of the magnitude.
    bool negative = false;
    std::string body = text;
    if (!body.empty() && (body[0] == '-' || body[0] == '+')) {
        negative = body[0] == '-';
        body.erase(0, 1);
    }
    if (body.empty()) return false;

    std::uint64_t magnitude = 0;
    if (parse_unsigned_integer(body, magnitude)) {
        const double magnitude_as_double = static_cast<double>(magnitude);
        result = negative ? -magnitude_as_double : magnitude_as_double;
        return true;
    }

    // protoc's ConsumeNumber (`parser.cc:231-255`): after an integer parse
    // failure, an octal/hex token is "Integer out of range", while a decimal
    // token can still be a floating value (`18446744073709551616` becomes
    // 1.8446744073709552e+19).
    if (body[0] == '0') return false;

    char* end = 0;
    errno = 0;
    const double parsed = std::strtod(body.c_str(), &end);
    if (end == body.c_str() || *end != '\0') return false;
    result = negative ? -parsed : parsed;
    return true;
}

std::string to_json_name(const std::string& name)
{
    bool capitalize_next = false;
    std::string result;
    result.reserve(name.size());
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char character = name[i];
        if (character == '_') {
            capitalize_next = true;
        } else if (capitalize_next) {
            result.push_back(character >= 'a' && character <= 'z'
                                 ? static_cast<char>(character - 'a' + 'A')
                                 : character);
            capitalize_next = false;
        } else {
            result.push_back(character);
        }
    }
    return result;
}

namespace {

std::string package_prefix(const SchemaFile& file)
{
    if (!file.file.has_package || file.file.package.empty()) return std::string();
    return "." + view_text(file.file.package);
}

SourceLocation declaration_location(const SchemaFile& file,
                                    const DescriptorPath& path)
{
    const std::map<DescriptorPath, SourceLocation>::const_iterator found =
        file.declaration_locations.find(path);
    if (found == file.declaration_locations.end()) return unknown_location();
    return found->second;
}

// Records one declaration in a protobuf lexical scope. Enum values use their
// containing scope rather than the enum type's scope, so all declaration
// categories share one table per scope. Returns false and fills error when
// the name is already taken. The error carries the redeclaration position
// when the file records one; descriptor-only errors name the descriptor
// path and use an unknown location instead of fabricated coordinates.
//
// A name already taken in this scope is the SAME defect as a collision between
// two files, so it carries the same dedicated code: E1/A3 map one lookup
// outcome to one code, not one code per producer, and a caller must never have
// to read a message text to tell a redeclaration from a generic failure. The
// message names both declarations by kind and name within the scope; the
// cross-file producer (add_symbol) additionally names both logical files.
bool report_scope_collision(const SchemaFile& file,
                            const str_view& name,
                            const char* kind,
                            const char* existing_kind,
                            const std::string& scope,
                            const DescriptorPath& path,
                            Diagnostic& error)
{
    std::string message = "name collision in " +
        (scope.empty() ? std::string("global scope") : scope) +
        ": " + std::string(kind) + " " + view_text(name) + " conflicts with " +
        existing_kind;
    SourceLocation location = declaration_location(file, path);
    if (location.line == 0) message += " at " + format_path(path);
    error.file = schema_file_label(file);
    error.location = location;
    error.message = message;
    error.warning = false;
    error.code = DIAGNOSTIC_DUPLICATE_SYMBOL;
    return false;
}

bool try_add_scope_name(std::map<str_view, const char*>& names,
                        const str_view& name,
                        const char* kind,
                        const char** existing_kind)
{
    const std::pair<std::map<str_view, const char*>::iterator, bool> inserted =
        names.insert(std::map<str_view, const char*>::value_type(name, kind));
    if (!inserted.second) {
        *existing_kind = inserted.first->second;
        return false;
    }
    return true;
}

bool validate_file_scope_names(const SchemaFile& file, const std::string& scope,
                               Diagnostic& error)
{
    std::map<str_view, const char*> names;
    const char* existing_kind = 0;
    for (std::size_t i = 0; i < file.file.enum_type.size(); ++i) {
        if (!try_add_scope_name(names, file.file.enum_type[i].name,
                                "enum type", &existing_kind)) {
            return report_scope_collision(
                file, file.file.enum_type[i].name, "enum type", existing_kind,
                scope, file_enum_path(static_cast<int>(i)), error);
        }
    }
    for (std::size_t i = 0; i < file.file.message_type.size(); ++i) {
        if (!try_add_scope_name(names, file.file.message_type[i].name,
                                "message", &existing_kind)) {
            return report_scope_collision(
                file, file.file.message_type[i].name, "message", existing_kind,
                scope, file_message_path(static_cast<int>(i)), error);
        }
    }
    for (std::size_t i = 0; i < file.file.service.size(); ++i) {
        if (!try_add_scope_name(names, file.file.service[i].name,
                                "service", &existing_kind)) {
            return report_scope_collision(
                file, file.file.service[i].name, "service", existing_kind,
                scope, file_service_path(static_cast<int>(i)), error);
        }
    }
    for (std::size_t i = 0; i < file.file.enum_type.size(); ++i) {
        for (std::size_t j = 0; j < file.file.enum_type[i].value.size(); ++j) {
            if (!try_add_scope_name(names, file.file.enum_type[i].value[j].name,
                                    "enum value", &existing_kind)) {
                return report_scope_collision(
                    file, file.file.enum_type[i].value[j].name, "enum value",
                    existing_kind, scope,
                    enum_value_path(file_enum_path(static_cast<int>(i)),
                                    static_cast<int>(j)),
                    error);
            }
        }
    }
    return true;
}

bool validate_message_scope_names(const SchemaFile& file,
                                  const DescriptorProto& message,
                                  const DescriptorPath& message_path,
                                  const std::string& parent,
                                  Diagnostic& error)
{
    const std::string scope = joined_name(parent, message.name);
    std::map<str_view, const char*> names;
    const char* existing_kind = 0;
    for (std::size_t i = 0; i < message.field.size(); ++i) {
        if (!try_add_scope_name(names, message.field[i].name,
                                "field", &existing_kind)) {
            return report_scope_collision(
                file, message.field[i].name, "field", existing_kind, scope,
                message_field_path(message_path, static_cast<int>(i)), error);
        }
    }
    for (std::size_t i = 0; i < message.nested_type.size(); ++i) {
        if (!try_add_scope_name(names, message.nested_type[i].name,
                                "nested message", &existing_kind)) {
            return report_scope_collision(
                file, message.nested_type[i].name, "nested message",
                existing_kind, scope,
                message_nested_path(message_path, static_cast<int>(i)), error);
        }
    }
    for (std::size_t i = 0; i < message.enum_type.size(); ++i) {
        if (!try_add_scope_name(names, message.enum_type[i].name,
                                "nested enum type", &existing_kind)) {
            return report_scope_collision(
                file, message.enum_type[i].name, "nested enum type",
                existing_kind, scope,
                message_enum_path(message_path, static_cast<int>(i)), error);
        }
    }
    for (std::size_t i = 0; i < message.oneof_decl.size(); ++i) {
        if (!try_add_scope_name(names, message.oneof_decl[i].name,
                                "oneof", &existing_kind)) {
            return report_scope_collision(
                file, message.oneof_decl[i].name, "oneof", existing_kind, scope,
                message_oneof_path(message_path, static_cast<int>(i)), error);
        }
    }
    for (std::size_t i = 0; i < message.enum_type.size(); ++i) {
        for (std::size_t j = 0; j < message.enum_type[i].value.size(); ++j) {
            if (!try_add_scope_name(names, message.enum_type[i].value[j].name,
                                    "enum value", &existing_kind)) {
                return report_scope_collision(
                    file, message.enum_type[i].value[j].name, "enum value",
                    existing_kind, scope,
                    enum_value_path(
                        message_enum_path(message_path, static_cast<int>(i)),
                        static_cast<int>(j)),
                    error);
            }
        }
    }
    for (std::size_t i = 0; i < message.nested_type.size(); ++i) {
        if (!validate_message_scope_names(file, message.nested_type[i],
                                          message_nested_path(message_path,
                                                              static_cast<int>(i)),
                                          scope, error)) {
            return false;
        }
    }
    return true;
}

bool add_symbol(const SchemaFile& file, SymbolIndex& symbols,
                const std::string& name, int kind, const DescriptorPath& path,
                Diagnostic& error);

bool index_package_scopes(SchemaFile& file, SymbolIndex& symbols,
                          const std::string& package, Diagnostic& error)
{
    // Index every scope component (".a" and ".a.b" for package "a.b") so a
    // package name conflicts with any other declaration of the same fully
    // qualified name. Every component shares the package declaration path
    // ([2], the FileDescriptorProto.package field), whose recorded position
    // locates cross-file package collisions.
    DescriptorPath package_path;
    package_path.push_back(2);
    std::string scope;
    std::size_t start = 0;
    while (start < package.size()) {
        std::size_t dot = package.find('.', start);
        if (dot == std::string::npos) dot = package.size();
        scope += ".";
        scope.append(package, start, dot - start);
        if (!add_symbol(file, symbols, scope, SYMBOL_PACKAGE,
                        package_path, error)) {
            return false;
        }
        start = dot + 1;
    }
    return true;
}

bool add_symbol(const SchemaFile& file, SymbolIndex& symbols,
                const std::string& name, int kind, const DescriptorPath& path,
                Diagnostic& error)
{
    // H-2 of the classical code review (fix round r5), extended by fix round
    // r10: the kind domain every lookup depends on. The head filter reads
    // `is_type_kind` / `is_aggregate_kind` (a message, an enum, a package
    // scope or a service may commit a compound head; only a message or an
    // enum may win a whole name) and the failure walk reads `is_type_kind`
    // plus the package question; all of it holds only while this is the
    // single point that writes a kind. Debug-only by construction (profile
    // trees compile with NDEBUG), which is why the guarantee is also stated
    // in `schema_semantics.hpp`'s SymbolKind comment and pinned by the
    // indexed-kind test.
    assert(kind == 0 || kind == SYMBOL_PACKAGE || kind == SYMBOL_SERVICE ||
           kind == FieldDescriptorProto::TYPE_MESSAGE ||
           kind == FieldDescriptorProto::TYPE_ENUM);
    SymbolEntry entry;
    entry.kind = kind;
    entry.owner = &file;
    entry.path = path;
    // One value-type temporary with the entry MOVED into it, and the node
    // built from an rvalue: `insert(std::make_pair(name, entry))` copied the
    // key and the descriptor path into the pair, again into the value type
    // `insert(const value_type&)` takes, and a third time into the node - the
    // only redundant copy the 2026-09-28 codegen performance audit's
    // index-phase numbers actually contain (the audit's `view_text` half is
    // SSO-sized and allocates nothing; measured, `build/plan4/fix-r19/`).
    //
    // `emplace` is the idiomatic C++11 spelling of this and is NOT usable:
    // the supported GCC 4.7 toolchain's libstdc++ has no `map::emplace`
    // (`D-18`'s class, measured before the edit in
    // `build/plan4/fix-r19/alloc/preflight-gcc47.txt`). This form compiles and
    // runs under GCC 4.7.3 and GCC 15 alike.
    const std::pair<std::map<std::string, SymbolEntry>::iterator, bool> inserted =
        symbols.symbols.insert(
            std::map<std::string, SymbolEntry>::value_type(name, std::move(entry)));
    if (!inserted.second) {
        const int existing_kind = inserted.first->second.kind;
        // Package scopes merge across files instead of conflicting.
        if (kind == SYMBOL_PACKAGE && existing_kind == SYMBOL_PACKAGE) return true;
        // Only messages and enums are types; every other indexed category
        // (fields, oneofs, enum values, services) reports a symbol collision.
        // A package node conflicting with any other declaration reports a
        // package collision. Either side decides the wording so the text
        // does not depend on indexing order.
        std::string message;
        if (kind == SYMBOL_PACKAGE || existing_kind == SYMBOL_PACKAGE) {
            message = "duplicate package name " + name;
        } else if (kind == FieldDescriptorProto::TYPE_MESSAGE ||
                   kind == FieldDescriptorProto::TYPE_ENUM ||
                   existing_kind == FieldDescriptorProto::TYPE_MESSAGE ||
                   existing_kind == FieldDescriptorProto::TYPE_ENUM) {
            message = "duplicate type name " + name;
        } else {
            message = "duplicate symbol name " + name;
        }
        // Both declarations are named, the way protoc's `"X" is already
        // defined in file "Y"` names the *other* file (Appendix C D1/D2):
        // error.file/error.location keep the redeclaration, which is the
        // file being indexed, and the message carries both logical file
        // names plus the position each declaration has. A descriptor-origin
        // file has no recorded position, so its path stands in for one.
        const SchemaFile* previous = inserted.first->second.owner;
        SourceLocation location = declaration_location(file, path);
        std::string text = message + " in \"" + schema_file_label(file) + "\"";
        if (location.line == 0 && !path.empty()) {
            text += " at " + format_path(path);
        }
        text += "; already defined in \"" + schema_file_label(*previous) + "\"";
        const SourceLocation previous_location =
            declaration_location(*previous, inserted.first->second.path);
        if (previous_location.line == 0 && !inserted.first->second.path.empty()) {
            text += " at " + format_path(inserted.first->second.path);
        }
        error.file = schema_file_label(file);
        error.location = location;
        error.message = text;
        error.warning = false;
        error.code = DIAGNOSTIC_DUPLICATE_SYMBOL;
        return false;
    }
    return true;
}

bool index_message(SchemaFile& file, SymbolIndex& symbols,
                   const DescriptorProto& message, const std::string& parent,
                   const DescriptorPath& message_path, Diagnostic& error)
{
    const std::string name = joined_name(parent, message.name);
    if (!add_symbol(file, symbols, name, FieldDescriptorProto::TYPE_MESSAGE,
                    message_path, error)) {
        return false;
    }
    for (std::size_t i = 0; i < message.field.size(); ++i) {
        if (!add_symbol(file, symbols, joined_name(name, message.field[i].name),
                        0, message_field_path(message_path, static_cast<int>(i)),
                        error)) {
            return false;
        }
    }
    for (std::size_t i = 0; i < message.oneof_decl.size(); ++i) {
        if (!add_symbol(file, symbols,
                        joined_name(name, message.oneof_decl[i].name),
                        0, message_oneof_path(message_path, static_cast<int>(i)),
                        error)) {
            return false;
        }
    }
    for (std::size_t i = 0; i < message.enum_type.size(); ++i) {
        const DescriptorPath enum_path =
            message_enum_path(message_path, static_cast<int>(i));
        const std::string enum_name =
            joined_name(name, message.enum_type[i].name);
        if (!add_symbol(file, symbols, enum_name, FieldDescriptorProto::TYPE_ENUM,
                        enum_path, error)) {
            return false;
        }
        for (std::size_t j = 0; j < message.enum_type[i].value.size(); ++j) {
            if (!add_symbol(file, symbols,
                            joined_name(name, message.enum_type[i].value[j].name),
                            0, enum_value_path(enum_path, static_cast<int>(j)),
                            error)) {
                return false;
            }
        }
    }
    for (std::size_t i = 0; i < message.nested_type.size(); ++i) {
        if (!index_message(file, symbols, message.nested_type[i], name,
                           message_nested_path(message_path, static_cast<int>(i)),
                           error)) {
            return false;
        }
    }
    return true;
}

// Field context for the resolver: the file that contains the field, the
// field's descriptor path, and the field itself. The synthetic-map-entry
// guard (Step 4.2) and the descriptor-origin declared-kind check (Step 5)
// both need the field, not only its enclosing scope.
struct ResolveContext
{
    const SchemaFile* owner;
    const DescriptorPath* field_path;
    const FieldDescriptorProto* field;

    ResolveContext() : owner(0), field_path(0), field(0) {}
};

// How a lookup ended. Each value maps to one diagnostic code, so no caller
// has to read a message text (Step 4.1): missing -> DIAGNOSTIC_UNRESOLVED_TYPE,
// invisible -> DIAGNOSTIC_INVISIBLE_TYPE, a declaration that is not a type ->
// DIAGNOSTIC_NOT_A_TYPE, a foreign synthetic map entry ->
// DIAGNOSTIC_MAP_ENTRY_TYPE. Only RESOLVE_OK carries a symbol.
enum ResolveOutcome
{
    RESOLVE_OK,
    RESOLVE_MISSING,
    RESOLVE_INVISIBLE,
    RESOLVE_NOT_A_TYPE,
    RESOLVE_MAP_ENTRY
};

// protoc's `Symbol::IsType()` (descriptor.cc:676): the declarations a field
// type may name, and therefore the only kinds that may win a WHOLE-name
// reference.
bool is_type_kind(int kind)
{
    return kind == FieldDescriptorProto::TYPE_MESSAGE ||
           kind == FieldDescriptorProto::TYPE_ENUM;
}

// protoc's `Symbol::IsAggregate()` (descriptor.cc:677-679): the declarations
// whose scope commits the first component of a COMPOUND relative name -
// IsType() plus a package scope plus a service. A service has no members, so
// a committed service head fails the member lookup, and that failure is what
// protoc reports for the shape (fix round r10, oracle case
// `head01_service_compound`: "Q.R" is resolved to "inner.Q.R", which is not
// defined).
bool is_aggregate_kind(int kind)
{
    return is_type_kind(kind) || kind == SYMBOL_PACKAGE ||
           kind == SYMBOL_SERVICE;
}

// The kinds that must carry a type_name: the named kinds a field can declare.
// protoc's cross-link refuses a field whose cpp_type is a message or an enum
// and whose descriptor carries no type_name (descriptor.cc's CrossLinkField:
// "Field with message or enum type missing type_name."), and a group is a
// message. Contract clause 12, ledger D-23 (fix round r11).
bool is_named_kind(int kind)
{
    return kind == FieldDescriptorProto::TYPE_MESSAGE ||
           kind == FieldDescriptorProto::TYPE_ENUM ||
           kind == FieldDescriptorProto::TYPE_GROUP;
}

// The kinds the descriptor model defines. A field whose declared kind is
// outside this set names a kind that does not exist: the reference compiler
// resolves an unknown value as the proto default (TYPE_DOUBLE) and emits
// working code, but this project's emitter has no spelling for it and used to
// write "???type" into the header (plan 5 follow-up S1, W-10).
bool is_known_field_kind(int kind)
{
    return kind >= FieldDescriptorProto::TYPE_DOUBLE &&
           kind <= FieldDescriptorProto::TYPE_SINT64;
}

// The field-number bounds both frontends enforce. The parser refuses these
// shapes as source text (proto_parser.cpp:1049-1060: 0, above the maximum,
// and the reserved range) and resolve_message refuses them on
// descriptor-origin input, so the two halves of the frontend parity rule
// (contract clause 12, D-23) watch the same numbers.
const std::int32_t kMaxFieldNumber = 536870911;
const std::int32_t kReservedFieldNumberFirst = 19000;
const std::int32_t kReservedFieldNumberLast = 19999;

// The largest EXCLUSIVE end an extension range may carry: one past the last
// extension number, `FieldDescriptor::kMaxNumber + 1`. protoc refuses a range
// whose end exceeds it (`ValidateExtensionRangeOptions`,
// descriptor.cc:8395-8404) except for a MessageSet, whose extension numbers are
// plain int32s (step B5, CR-07).
const std::int32_t kMaxExtensionEnd = kMaxFieldNumber + 1;

std::string number_text(std::int32_t number)
{
    std::ostringstream output;
    output << number;
    return output.str();
}

// Splits a relative reference into its first component and the remainder.
void split_reference(const std::string& raw, std::string& first, std::string& rest)
{
    const std::size_t dot = raw.find('.');
    if (dot == std::string::npos) {
        first = raw;
        rest.clear();
        return;
    }
    first = raw.substr(0, dot);
    rest = raw.substr(dot + 1);
}

// The candidate first component one scope contributes. protoc tries the first
// component of a relative name in every enclosing scope, the global scope
// last. One function serves the winning walk and the failure walk, so the two
// cannot drift apart.
std::string scope_candidate(const std::string& scope, const std::string& first)
{
    return scope.empty() ? "." + first : scope + "." + first;
}

// Steps one lexical scope outward. Returns false at the global scope (the
// empty string), which is the last scope tried.
bool enclosing_scope(const std::string& scope, std::string& outer)
{
    if (scope.empty()) return false;
    const std::size_t dot = scope.rfind('.');
    if (dot == std::string::npos || dot == 0) outer.clear();
    else outer = scope.substr(0, dot);
    return true;
}

// A package scope is visible when a VISIBLE file's package is the head itself
// or lies under it: `package a.b` covers the head `.a`, because protoc's
// reference rule is a prefix test with a dot boundary (`descriptor.cc`
// `IsInPackage`, kept by the code-review lane as
// `build/code-review-2d/descriptor.cc`). The dot boundary is load-bearing:
// `package foobar` must NOT cover `.foo` (oracle b02). Ownership still does
// not decide the answer - package scopes merge across files, so the recorded
// owner is just the first file indexed - and the question is asked about the
// VISIBLE set only, so an unimported declarer still covers nothing.
//
// `package_head` is a dotted absolute spelling (`.a.b`): every caller reaches
// this function through `candidate_is_visible`, whose `candidate` argument is
// the index's own spelling - `raw` under the `raw[0] == '.'` guard,
// `scope_candidate`'s head, or `winning_head + "." + rest` - so the leading
// dot and the non-empty head are preconditions, not cases (W-53, re-measured
// after S7b).
//
// The equality form this function carried before fix round r5 came from
// `descriptor.cc`'s `FindSymbol`, whose package special case answers which
// file OWNS the merged node; the rule a lookup needs is `IsInPackage`, and
// the two differ exactly for a reference headed by a parent package.
//
// Measured with libprotoc 30.1. The relative-reference group r01..r07 of the
// code-review-2d corpus (`build/plan4/fix-r5/oracle/`, re-run by fix round
// r5): a reference whose first component is a package declared only as a
// parent prefix of a visible file's package is ACCEPTED when the member
// exists (r01..r04, r06 - source exit 0, descriptor validator exit 0) and
// rejected with "is not defined" when it does not (r05); no import at all
// still rejects (r08). The step-4 group (F1a/F1b/F4/F5) survives intact: the
// unimported file may declare `package shared` (F1) or only the sub-package
// `package shared.deep` (F4) without breaking the accept, in both root
// orders, while F5's shape - only a visible sub-package declarer and no
// visible file declaring the head's own package - is rejected, and protoc
// words that rejection "is not defined": the sub-package declarer covers the
// head, the member is absent, and the committed head yields the not-defined
// class, not the not-visible one (b01).
bool package_is_visible(const std::set<const SchemaFile*>& visible_files,
                        const std::string& package_head)
{
    const str_view head_pkg = str_view(package_head).substr(1);
    for (std::set<const SchemaFile*>::const_iterator it = visible_files.begin();
         it != visible_files.end(); ++it) {
        const str_view& declared = (*it)->file.package;
        if (declared == head_pkg) return true;
        if (declared.size() > head_pkg.size() &&
            declared.substr(0, head_pkg.size()) == head_pkg &&
            declared[head_pkg.size()] == '.') {
            return true;
        }
    }
    return false;
}

// True when the candidate first component is visible from the requesting file.
bool candidate_is_visible(const SymbolEntry& entry,
                          const std::set<const SchemaFile*>& visible_files,
                          const std::string& candidate)
{
    if (entry.kind == SYMBOL_PACKAGE) {
        return package_is_visible(visible_files, candidate);
    }
    return visible_files.find(entry.owner) != visible_files.end();
}

// True when the entry descriptor is nested directly in the referencing
// field's own message. The field's path is message_path + [2, index] and the
// entry's is message_path + [3, index], so the two share the container prefix
// and differ only in the declaration slot.
bool nested_in_field_message(const ResolveContext& context,
                             const DescriptorPath& entry_path)
{
    if (context.field_path == 0) return false;
    const DescriptorPath& field_path = *context.field_path;
    if (entry_path.size() != field_path.size() || entry_path.size() < 2) {
        return false;
    }
    for (std::size_t i = 0; i + 2 < entry_path.size(); ++i) {
        if (entry_path[i] != field_path[i]) return false;
    }
    return entry_path[entry_path.size() - 2] == 3 &&
           field_path[field_path.size() - 2] == 2;
}

// protoc's ownership rule for a synthetic map entry (A22, measured) is
// `internal::owns_map_entry`, declared in schema_semantics_internal.hpp and
// defined beside the other seam rules below: the rule is a precondition that
// includes the entry's own marker, so a direct test can prove a real nested
// message is not claimed as an entry (W-49).
//
// True when the resolved symbol is a synthetic map entry owned by some other
// field, so naming it is a user attempt to use an entry as a field type.
bool is_foreign_map_entry(const ResolveContext& context, const SymbolEntry& entry)
{
    if (entry.kind != FieldDescriptorProto::TYPE_MESSAGE) return false;
    const DescriptorProto* entry_message = find_message(*entry.owner, entry.path);
    if (entry_message == 0) return false;
    if (!entry_message->options.has_map_entry || !entry_message->options.map_entry) {
        return false;
    }
    // A method endpoint resolves with no field context. Nothing can own the
    // entry there, and protoc's ownership rule is a FIELD rule: `ValidateMapEntry`
    // is reached only from `ValidateFieldOptions`, so a method endpoint naming a
    // synthetic entry is an ordinary message type to the reference compiler
    // (measured: `map_entry_method.pbs` is accepted by libprotoc 30.1). Only a
    // field that does not own the entry is foreign (CR-21).
    if (context.field == 0 || context.owner == 0 || context.field_path == 0) {
        return false;
    }
    return !internal::owns_map_entry(*context.owner, *context.field_path,
                                      *context.field, entry, *entry_message);
}

// Implements protoc's lexical name lookup over the compilation-wide index.
//
// An absolute name is looked up whole, with visibility still applied. A
// relative name resolves its FIRST component from the innermost enclosing
// scope outward, and the walk continues past a candidate that is invisible
// (B6/B10) or that cannot take the name (B8/B13): visibility filters the
// walk instead of judging its result (A9). What "can take the name" means is
// TWO rules, because protoc's own predicates differ (`descriptor.cc:676-679`,
// `LookupSymbolNoPlaceholder` at `descriptor.cc:5046-5112`): a COMPOUND
// reference commits on an AGGREGATE head - a message, an enum, a package
// scope, or a service - so the remainder must resolve strictly underneath it
// with no outward fallback (B1/B7/B11/B12/B14), while a WHOLE-name reference
// lands on the name itself, so only a TYPE (a message or an enum) wins and
// every other declaration keeps the walk moving outward (B8/B13 for fields
// and enum values, fix round r10's `head03`/`head04` oracle cases for
// services and package scopes). Before fix round r10 a single `kind != 0`
// test served both shapes, so a package won a whole name (a false rejection
// when an outer type existed) and a service never committed a compound name
// (a false acceptance when an outer type carried the member); `D-21`.
//
// Visibility is always the same question about the symbol the name lands on: a
// package node is visible when ANY visible file declares exactly that package
// (`package_is_visible`, R4), every other declaration when its own file is
// visible. Neither spelling of a name can change that answer, in the whole-name
// branch or in the compound-member branch (D-06: both branches used to test the
// node's recorded owner, so a merged package node answered INVISIBLE where the
// relative walk answered NOT_A_TYPE).
//
// A member that the index does know but that is not a type (a field, an enum
// value, a oneof, a package scope) is RESOLVE_NOT_A_TYPE, the same category
// the absolute spelling of that declaration reports; only a member the index
// does not know at all is RESOLVE_MISSING (B12/B14). The oracle separates the
// two the same way - "is not a type" for `Holder.not_a_type`, `Holder.pick`
// and `Holder.V`, "resolved to ... which is not defined" for `Holder.E.V`,
// whose value is indexed in the containing scope rather than under the enum.
//
// When nothing wins, a second walk over the same candidates decides why, and
// it asks VISIBILITY first: a candidate the requesting file cannot see is
// RESOLVE_INVISIBLE whatever its kind (R4; protoc's own order, because
// FindSymbol filters by dependency before the caller may look at the symbol's
// kind), and a candidate it CAN see which is not a type is RESOLVE_NOT_A_TYPE
// (B9/B15 - pass 1 would have returned any visible candidate that can take the
// reference, so a visible candidate here is always a non-type, exactly as in
// the absolute branch). A reference the index does not know at all is
// RESOLVE_MISSING (B5). Every outcome except RESOLVE_OK is named, never
// guessed from a message text, and the reason is recorded in *outcome.
//
// Fix round r17 (the 0927-mimo review's F8) moved the failure walk's order: it
// used to answer NOT_A_TYPE for a nearer non-type that was not a package even
// when its file was invisible, so ONE declaration got two codes depending on
// the spelling (the relative `VALUE` answered NOT_A_TYPE, the absolute
// `.p.VALUE` answered INVISIBLE) - ledger `D-06`'s class, whose own rule is
// "visibility is asked before the kind is classified". Review round r5's H-1
// had pinned the old order deliberately; the oracle measurement supersedes it
// in the direction of the absolute branch and of protoc's filter.
//
// *matched_name carries the fully qualified name the diagnostic must name:
// the resolved symbol on success, the resolved compound path when its member
// is absent (that is what makes the shadowing diagnostic name
// ".pkg.Encloser.Choice.Item" instead of the raw text), or the candidate the
// failure walk stopped at. It stays empty when there is no candidate at all.
const SymbolEntry* resolve_name(const std::string& raw, const std::string& scope,
                                const SymbolIndex& symbols,
                                const std::set<const SchemaFile*>& visible_files,
                                const ResolveContext& context,
                                ResolveOutcome* outcome, std::string* matched_name)
{
    *outcome = RESOLVE_MISSING;
    matched_name->clear();
    if (!raw.empty() && raw[0] == '.') {
        const std::map<std::string, SymbolEntry>::const_iterator found =
            symbols.symbols.find(raw);
        if (found == symbols.symbols.end()) return 0;
        *matched_name = raw;
        // Every name this function tests is the index's dotted absolute
        // spelling (`scope_candidate` builds them that way, and
        // `package_prefix` returns a dotted package), so R4's question is asked
        // with the name itself - the recorded-owner test gave a merged package
        // node two codes depending on the spelling (D-06).
        if (!candidate_is_visible(found->second, visible_files, raw)) {
            *outcome = RESOLVE_INVISIBLE;
            return 0;
        }
        // A package scope and a service are lookup aggregates but never
        // types; so is every other kind-excluded declaration.
        if (!is_type_kind(found->second.kind)) {
            *outcome = RESOLVE_NOT_A_TYPE;
            return 0;
        }
        if (is_foreign_map_entry(context, found->second)) {
            *outcome = RESOLVE_MAP_ENTRY;
            return 0;
        }
        *outcome = RESOLVE_OK;
        return &found->second;
    }

    // A relative reference whose last component is empty (a trailing dot) can
    // never name a symbol: every spelling in the index is built from real
    // names. protoc walks it as a compound name and refuses it - `"N." is
    // resolved to "p.N.", which is not defined` (measured, fix round r17) -
    // while `split_reference("N.")` returns an empty remainder, which used to
    // classify the raw text as a WHOLE-name reference to `N`, so a message of
    // that name LINKED where protoc refuses (the 0927-mimo review's F7: a false
    // accept on crafted descriptor input, not merely a silent classification).
    // The absolute spelling already lands here as RESOLVE_MISSING (`".p.N."`
    // is not indexed), so both spellings answer one code; *matched_name stays
    // empty so the refusal names the raw text.
    if (!raw.empty() && raw[raw.size() - 1] == '.') return 0;

    std::string first;
    std::string rest;
    split_reference(raw, first, rest);

    // Pass 1: the innermost VISIBLE candidate that can take the name wins
    // the head. Which candidates can is the two-filter rule the doc comment
    // above states: an aggregate for a compound reference, a type for a
    // whole-name reference (fix round r10).
    const bool compound_reference = !rest.empty();
    std::string candidate_scope = scope;
    std::string winning_head;
    const SymbolEntry* winner = 0;
    for (;;) {
        const std::string head = scope_candidate(candidate_scope, first);
        const std::map<std::string, SymbolEntry>::const_iterator found =
            symbols.symbols.find(head);
        if (found != symbols.symbols.end() &&
            (compound_reference ? is_aggregate_kind(found->second.kind)
                                : is_type_kind(found->second.kind)) &&
            candidate_is_visible(found->second, visible_files, head)) {
            winning_head = head;
            winner = &found->second;
            break;
        }
        std::string outer;
        if (!enclosing_scope(candidate_scope, outer)) break;
        candidate_scope = outer;
    }

    if (winner != 0) {
        if (rest.empty()) {
            // The whole-name filter admits only types already; the check is
            // kept so that a future edit of that filter cannot land a
            // non-type symbol in a field's `type` (fix round r10).
            if (!is_type_kind(winner->kind)) {
                *outcome = RESOLVE_NOT_A_TYPE;
                *matched_name = winning_head;
                return 0;
            }
            if (is_foreign_map_entry(context, *winner)) {
                *outcome = RESOLVE_MAP_ENTRY;
                *matched_name = winning_head;
                return 0;
            }
            *outcome = RESOLVE_OK;
            *matched_name = winning_head;
            return winner;
        }
        const std::string compound = winning_head + "." + rest;
        const std::map<std::string, SymbolEntry>::const_iterator member =
            symbols.symbols.find(compound);
        if (member == symbols.symbols.end()) {
            // The matched aggregate commits the whole name: no outer scope is
            // tried again (R3), and the diagnostic names this path.
            *matched_name = compound;
            return 0;
        }
        *matched_name = compound;
        // A package member asks the same question: the merged node is visible
        // when ANY visible file declares exactly this package, so a package
        // reached through a committed head answers one code in both spellings
        // (D-06, the compound half of the same finding).
        if (!candidate_is_visible(member->second, visible_files, compound)) {
            *outcome = RESOLVE_INVISIBLE;
            return 0;
        }
        if (!is_type_kind(member->second.kind)) {
            // Found, but not a type (a field, an enum value, a oneof, a
            // service, or a package scope): mirror the whole-name branch
            // above instead of collapsing it into "not defined", which would
            // give the same declaration two codes depending on how it is
            // spelled (E1/A3). `*matched_name` already names the compound
            // path, so the diagnostic still says which member was reached.
            *outcome = RESOLVE_NOT_A_TYPE;
            return 0;
        }
        if (is_foreign_map_entry(context, member->second)) {
            *outcome = RESOLVE_MAP_ENTRY;
            return 0;
        }
        *outcome = RESOLVE_OK;
        return &member->second;
    }

    // Pass 2: nothing won, so classify the failure. The same scope walk runs
    // again without the visibility filter; the first candidate the index
    // knows decides between "not a type" and "not visible". A candidate the
    // walk would have accepted if it were visible cannot appear here: pass 1
    // would have returned it.
    candidate_scope = scope;
    for (;;) {
        const std::string head = scope_candidate(candidate_scope, first);
        const std::map<std::string, SymbolEntry>::const_iterator found =
            symbols.symbols.find(head);
        if (found != symbols.symbols.end()) {
            // Visibility first, then the kind (fix round r17, the review's
            // F8): this is the order the absolute branch asks, the order the
            // compound-member branch asks, and protoc's own order - FindSymbol
            // filters a symbol whose file is not a dependency BEFORE its
            // caller may look at the kind (descriptor.cc:5019-5043), which is
            // why a service in an unimported file answers its
            // undeclared-dependency class in both spellings (measured this
            // round: `"p.S" seems to be defined in "dep.proto", which is not
            // imported by "root.proto"`). A VISIBLE candidate reached here is
            // always a non-type - pass 1 would have returned any visible
            // candidate that can take this reference - so it keeps
            // NOT_A_TYPE, which is what lets the whole-name filter skip a
            // visible package (D-21's mirror half) and still name it a
            // non-type.
            const bool visible = candidate_is_visible(found->second,
                                                      visible_files, head);
            *outcome = visible ? RESOLVE_NOT_A_TYPE : RESOLVE_INVISIBLE;
            *matched_name = head;
            return 0;
        }
        std::string outer;
        if (!enclosing_scope(candidate_scope, outer)) break;
        candidate_scope = outer;
    }
    return 0;   // RESOLVE_MISSING, with no candidate to name
}

void warn_once(SchemaFile& file, const std::string& message,
               DiagnosticCode code, const SourceLocation& location,
               const DescriptorPath& path)
{
    // Dedupe by identity, not just text: identical messages for different
    // fields (for example two fields of one unresolved imported type) must
    // each warn once per file, while repeated resolution passes must not
    // duplicate any of them. Fields that share one unknown source position
    // (descriptor-only files record no per-field locations) are told apart
    // by appending their descriptor path, following the fail_at convention;
    // source warning texts stay unchanged.
    std::string stored = message;
    if (location.line == 0) stored += " at " + format_path(path);
    for (std::size_t i = 0; i < file.warnings.size(); ++i) {
        const Diagnostic& existing = file.warnings[i];
        if (existing.code == code && existing.message == stored &&
            existing.location.offset == location.offset &&
            existing.location.line == location.line &&
            existing.location.column == location.column) {
            return;
        }
    }
    Diagnostic diagnostic;
    diagnostic.file = schema_file_label(file);
    diagnostic.location = location;
    diagnostic.warning = true;
    diagnostic.message = stored;
    diagnostic.code = code;
    file.warnings.push_back(diagnostic);
}

SourceLocation field_type_location(const SchemaFile& file,
                                   const DescriptorPath& field_path)
{
    const FieldSource* source = find_field_source(file, field_path);
    if (source == 0 || source->type_location.line == 0) return unknown_location();
    return source->type_location;
}

SourceLocation field_default_location(const SchemaFile& file,
                                      const DescriptorPath& field_path)
{
    const FieldSource* source = find_field_source(file, field_path);
    if (source == 0 || source->default_location.line == 0) return unknown_location();
    return source->default_location;
}

SourceLocation field_packed_location(const SchemaFile& file,
                                     const DescriptorPath& field_path)
{
    const FieldSource* source = find_field_source(file, field_path);
    if (source == 0 || source->packed_location.line == 0) return unknown_location();
    return source->packed_location;
}

std::string raw_type_name(const SchemaFile& file,
                          const FieldDescriptorProto& field,
                          const DescriptorPath& field_path)
{
    const FieldSource* source = find_field_source(file, field_path);
    if (source != 0 && !source->raw_type_name.empty()) return source->raw_type_name;
    if (field.has_type_name) return view_text(field.type_name);
    return std::string();
}

// The one place every fatal diagnostic is built: unknown positions fall back
// to the descriptor path, and the caller chooses the code. The resolvable
// failures below carry their own code instead of DIAGNOSTIC_GENERIC, because a
// caller matching on message text is not a contract (A3).
bool fail_at_code(const SchemaFile& file, const std::string& message,
                  const SourceLocation& location, const DescriptorPath& path,
                  DiagnosticCode code, Diagnostic& error)
{
    std::string text = message;
    // An empty path is not a location: rendering it as " at []" would put an
    // empty descriptor path into the message (ledger `D-59`). Every caller that
    // passes a real path keeps the suffix.
    if (location.line == 0 && !path.empty()) text += " at " + format_path(path);
    error.file = schema_file_label(file);
    error.location = location;
    error.message = text;
    error.warning = false;
    error.code = code;
    return false;
}

bool fail_at(const SchemaFile& file, const std::string& message,
             const SourceLocation& location, const DescriptorPath& path,
             Diagnostic& error)
{
    return fail_at_code(file, message, location, path, DIAGNOSTIC_GENERIC, error);
}

// True when `name` is a legal protobuf symbol name in protoc's own sense
// (`ValidateSymbolName`, descriptor.cc:5419-5438): non-empty and every character
// in `[A-Za-z0-9_]`. The reference imposes NO first-character rule - it accepts a
// name starting with a digit - so neither does this: whether a name is usable as
// a C++ name is the generator's question, not the linker's (contract clause 11,
// and the same lesson `P-19`/F3 recorded for the field-kind rule).
bool is_valid_symbol_name(const str_view& name)
{
    if (name.empty()) return false;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char character = name[i];
        const bool letter = (character >= 'a' && character <= 'z') ||
                            (character >= 'A' && character <= 'Z');
        const bool digit = character >= '0' && character <= '9';
        if (!letter && !digit && character != '_') return false;
    }
    return true;
}

// True when `text` is an IDENTIFIER in the reference compiler's sense, which
// is stricter than the symbol-name rule above: the first character must be a
// letter or an underscore, the rest may also be digits
// (`io::Tokenizer::IsIdentifier`, whose character classes are ASCII-only -
// `Letter` is `[A-Za-z_]`, `Alphanumeric` is `[A-Za-z0-9_]`).
//
// The reference compiler uses this rule for the default value of an enum
// field ("Default value for an enum field must be an identifier.",
// descriptor.cc's enum branch of the descriptor builder), and it asks it
// BEFORE it asks whether the default names a value of the enum: a default of
// "1A" is a legal symbol name, so the membership check alone would let it
// through and the emitter would write `::p::E::1A`, which is not C++.
bool is_identifier(const std::string& text)
{
    if (text.empty()) return false;
    const char first = text[0];
    const bool first_is_letter =
        (first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') ||
        first == '_';
    if (!first_is_letter) return false;
    for (std::size_t i = 1; i < text.size(); ++i) {
        const char character = text[i];
        const bool letter = (character >= 'a' && character <= 'z') ||
                            (character >= 'A' && character <= 'Z');
        const bool digit = character >= '0' && character <= '9';
        if (!letter && !digit && character != '_') return false;
    }
    return true;
}

// every kind reports the same category with the same code (E1/A3). The message
// mirrors protoc's two wordings - an empty name is "missing", an illegal
// character is "not a valid identifier" - while the code stays DIAGNOSTIC_
// INVALID_SYMBOL_NAME for both, because they are one category: the name is not a
// symbol name. `scope` names the containing scope in the message; the location
// and descriptor path are the declaration's own.
// `name` is borrowed, not owned: every caller passes either a `view_text(...)`
// temporary - whose lifetime covers the call - or a view into the descriptor,
// and the name is only read here.  Taking a std::string forced the one caller
// that had the view in hand to materialize a copy of it first.  On the legacy
// lane `str_view` IS `std::string`, so this parameter is what it always was.
bool fail_symbol_name(const SchemaFile& file, const str_view& name,
                      const char* kind, const std::string& scope,
                      const DescriptorPath& path, Diagnostic& error)
{
    const std::string where =
        scope.empty() ? std::string("the global scope") : scope;
    std::string message;
    if (name.empty()) {
        message = std::string(kind) + " in " + where + " has no name";
    } else {
        message = std::string(kind) + " name \"" + std::string(name) +
                  "\" in " + where + " is not a valid identifier";
    }
    return fail_at_code(file, message, declaration_location(file, path), path,
                        DIAGNOSTIC_INVALID_SYMBOL_NAME, error);
}

bool check_symbol_name(const SchemaFile& file, const str_view& name,
                       const char* kind, const std::string& scope,
                       const DescriptorPath& path, Diagnostic& error)
{
    if (is_valid_symbol_name(name)) return true;
    return fail_symbol_name(file, name, kind, scope, path, error);
}

// One message the nesting walk has not visited yet.
struct PendingMessage
{
    const DescriptorProto* message;
    int depth;
    DescriptorPath path;
};

// The depth of the deepest message the file declares, or 0 when it declares no
// message at all. The walk uses an explicit stack, so it never recurses, and
// it stops at the limit, so the depth it measures never exceeds the limit it
// enforces: a descriptor that nests deeper is reported, not descended into.
//
// The path of the message that is too deep is stored in `offending_path` (left
// empty when the file is within the limit).
int deepest_message_nesting(const FileDescriptorProto& file,
                             DescriptorPath& offending_path)
{
    const int limit = max_message_nesting_depth();
    std::vector<PendingMessage> pending;
    for (std::size_t i = 0; i < file.message_type.size(); ++i) {
        PendingMessage entry;
        entry.message = &file.message_type[i];
        entry.depth = 1;
        entry.path = file_message_path(static_cast<int>(i));
        pending.push_back(entry);
    }
    while (!pending.empty()) {
        const PendingMessage entry = pending.back();
        pending.pop_back();
        if (entry.depth >= limit) {
            offending_path = entry.path;
            return entry.depth;
        }
        for (std::size_t i = 0; i < entry.message->nested_type.size(); ++i) {
            PendingMessage nested;
            nested.message = &entry.message->nested_type[i];
            nested.depth = entry.depth + 1;
            nested.path = message_nested_path(entry.path, static_cast<int>(i));
            pending.push_back(nested);
        }
    }
    return 0;
}

// The recursive half of validate_symbol_names: the message's own name, then its
// fields, nested messages, nested enums (with their values) and oneofs, in the
// order validate_message_scope_names uses, so a file with several malformed
// names reports the first one in declaration order.
bool validate_message_symbol_names(const SchemaFile& file,
                                   const DescriptorProto& message,
                                   const DescriptorPath& message_path,
                                   const std::string& parent,
                                   Diagnostic& error)
{
    const std::string scope = joined_name(parent, message.name);
    if (!check_symbol_name(file, message.name, "message", parent,
                           message_path, error)) {
        return false;
    }
    for (std::size_t i = 0; i < message.field.size(); ++i) {
        if (!is_valid_symbol_name(message.field[i].name)) {
            return fail_symbol_name(
                file, message.field[i].name, "field", scope,
                message_field_path(message_path, static_cast<int>(i)), error);
        }
    }
    for (std::size_t i = 0; i < message.nested_type.size(); ++i) {
        if (!validate_message_symbol_names(
                file, message.nested_type[i],
                message_nested_path(message_path, static_cast<int>(i)), scope,
                error)) {
            return false;
        }
    }
    for (std::size_t i = 0; i < message.enum_type.size(); ++i) {
        if (!is_valid_symbol_name(message.enum_type[i].name)) {
            return fail_symbol_name(
                file, message.enum_type[i].name, "nested enum type", scope,
                message_enum_path(message_path, static_cast<int>(i)), error);
        }
        for (std::size_t j = 0; j < message.enum_type[i].value.size(); ++j) {
            if (!is_valid_symbol_name(message.enum_type[i].value[j].name)) {
                return fail_symbol_name(
                    file, message.enum_type[i].value[j].name, "enum value",
                    scope,
                    enum_value_path(
                        message_enum_path(message_path, static_cast<int>(i)),
                        static_cast<int>(j)),
                    error);
            }
        }
    }
    for (std::size_t i = 0; i < message.oneof_decl.size(); ++i) {
        if (!is_valid_symbol_name(message.oneof_decl[i].name)) {
            return fail_symbol_name(
                file, message.oneof_decl[i].name, "oneof", scope,
                message_oneof_path(message_path, static_cast<int>(i)), error);
        }
    }
    return true;
}

// The diagnostic code one lookup outcome maps to (Step 4.1's table). Every
// enumerator is listed explicitly and there is no `default:` label on purpose:
// a new outcome added to ResolveOutcome then has to be given a code here
// instead of silently reading as "unresolved type".
DiagnosticCode outcome_code(ResolveOutcome outcome)
{
    switch (outcome) {
        case RESOLVE_MISSING: return DIAGNOSTIC_UNRESOLVED_TYPE;
        case RESOLVE_INVISIBLE: return DIAGNOSTIC_INVISIBLE_TYPE;
        case RESOLVE_NOT_A_TYPE: return DIAGNOSTIC_NOT_A_TYPE;
        case RESOLVE_MAP_ENTRY: return DIAGNOSTIC_MAP_ENTRY_TYPE;
        case RESOLVE_OK:
            // Not a failure: the caller asks for a code only when the lookup
            // failed. Kept so the switch stays exhaustive.
            return DIAGNOSTIC_GENERIC;
    }
    return DIAGNOSTIC_GENERIC;
}

// Names the declaration category of an indexed non-type symbol, so a "not a
// type" diagnostic can say what the name actually is. The index stores kind 0
// for every non-type declaration, so the category comes from the descriptor
// path the index recorded.
std::string declaration_category(const SchemaFile& file,
                                 const DescriptorPath& path, int kind)
{
    if (kind == SYMBOL_PACKAGE) return "a package scope";
    if (kind == SYMBOL_SERVICE) return "a service";
    if (path.size() >= 2 && path[path.size() - 2] == 8) return "a oneof";
    if (path.size() >= 2 && path[path.size() - 2] == 2) {
        const DescriptorPath container(path.begin(), path.end() - 2);
        if (find_enum(file, container) != 0) return "an enum value";
        if (find_message(file, container) != 0) return "a field";
        return "a declaration";
    }
    if (path.size() == 2 && path[0] == 6) return "a service";
    return "a declaration";
}

// The logical name of the file that defines an indexed symbol, for the
// diagnostics that must name it.
std::string defining_file_name(const SymbolIndex& symbols, const std::string& name)
{
    const std::map<std::string, SymbolEntry>::const_iterator found =
        symbols.symbols.find(name);
    if (found == symbols.symbols.end()) return std::string();
    return view_text(found->second.owner->file.name);
}

// The declared kind of a descriptor field, named for the A7 mismatch message.
// Only messages and enums are definitions a field type may name, so the two
// names below are the ones a legal descriptor carries; anything else is named
// by its numeric kind instead of being guessed at.
std::string descriptor_kind_name(int kind)
{
    if (kind == FieldDescriptorProto::TYPE_MESSAGE) return "a message";
    if (kind == FieldDescriptorProto::TYPE_ENUM) return "an enum";
    if (kind == FieldDescriptorProto::TYPE_GROUP) return "a group";
    std::ostringstream text;
    text << "kind " << kind;
    return text.str();
}

// A7's sentence: the field, the definition it names, the kind the descriptor
// declares it with, and the kind the definition actually has.
std::string kind_mismatch_message(const FieldDescriptorProto& field,
                                  const std::string& resolved_name, int kind)
{
    return "field \"" + view_text(field.name) + "\" declares \"" +
           resolved_name + "\" as " + descriptor_kind_name(field.type) +
           " but it is defined as " + descriptor_kind_name(kind) +
           ": descriptor metadata is authoritative and is not rewritten";
}

// A26: the fully qualified name of a message that carries the synthetic
// map-entry marker, used in the failure text. The walk is recursive because an
// entry is normally a nested type, and the whole declaration chain must appear
// in the name - the generator's own message is fully qualified, and
// tests/codegen/maps/test_codegen_maps.cmake matches the sentence with
// string(FIND), so the simple `BrokenMap.ItemsEntry` spelling would satisfy a
// descriptor-path assertion and still break that test.
//
// The failure is deliberately NOT built through fail_at: that helper appends
// " at <descriptor path>" when the file records no location, which is exactly
// the descriptor-origin case an entry without a value member comes from, and
// this sentence has to stay verbatim. No location suffix, no rewording: the
// text is what lets plan 5 delete the generator's own check without touching
// the CLI-visible test.
bool fail_malformed_map_entry(const SchemaFile& file,
                              const std::string& entry_name, Diagnostic& error)
{
    error = Diagnostic();
    error.file = schema_file_label(file);
    error.location = unknown_location();
    error.message = "Malformed map entry " + entry_name +
                    ": fields 1 and 2 are required";
    error.warning = false;
    error.code = DIAGNOSTIC_MALFORMED_MAP_ENTRY;
    return false;
}

// The shape failures of A26's sweep other than the missing-member one above
// (ledger `D-53`): the entry exists but does not match the key/value pair
// protoc's `ValidateMapEntry` requires. The code is the same category
// (`DIAGNOSTIC_MALFORMED_MAP_ENTRY` - an entry not of the required shape),
// while A26's exact sentence stays with that helper, because the generator's
// CLI-visible check matches its text.
bool fail_map_entry_shape(const SchemaFile& file, const std::string& entry_name,
                          const std::string& requirement, Diagnostic& error)
{
    return fail_at_code(file,
                        "Malformed map entry " + entry_name + ": " +
                            requirement + " is required",
                        unknown_location(), DescriptorPath(),
                        DIAGNOSTIC_MALFORMED_MAP_ENTRY, error);
}

// The one sentence validate_default's unknown-kind branch records. It lives
// here, beside the call that writes it, so the predicate below and the
// producer cannot drift and no other translation unit has to match on message
// text (fix round r4, ledger D-13).
//
// The prefix is load-bearing by prefix-match, so it is also a boundary: any
// diagnostic whose text merely STARTS with this sentence would be dropped by
// A10's hygiene (the seeded-prefix measurement of the bunny review, ledger
// D-27), and no other diagnostic may begin with these words. The companion
// code review proposed a dedicated code or a paired-warning id instead; both
// are contract amendments (E1's appended-codes rule), declined for now - the
// text contract lives in these comments and is the change any new sentence
// must clear.
const std::string kDeferredDefaultPrefix =
    "cannot validate default for unresolved imported type ";

bool validate_message_map_entries(const SchemaFile& file,
                                  const DescriptorProto& message,
                                  const std::string& qualified_name,
                                  Diagnostic& error)
{
    // The marker is the only thing consulted, so the check runs whether or not
    // anything references the entry: an unreferenced entry never enters a
    // lookup, and a reference-driven check would accept the corpus fixture
    // `tests/codegen/maps/data/malformed_map.pbs`.
    if (message.options.has_map_entry && message.options.map_entry) {
        // D-53 (fix round r25) and D-62 (fix round r26): ask protoc's
        // `ValidateMapEntry` (descriptor.cc:8445-8478) for every clause a
        // descriptor alone can answer - its first group, "must not contain
        // extensions, extension range or nested message or enums" (:8448-8452),
        // and the key/value shape (:8462-8471) - not merely the two field
        // numbers. protoc applies that rule to every field whose type is a map
        // entry, so a crafted descriptor's synthetic entry used to link in both
        // modes with a bytes/float/enum key, a renamed key or value, a repeated
        // member, a third field, a nested declaration, or an extension range.
        // The reference-free domain is kept on purpose - the shape is provable
        // from the descriptor alone, which is A26's own reason for running this
        // before resolution - and the key-type rule reuses the very helper the
        // parser calls (`valid_map_key_type`), so the two frontends agree.
        // What this sweep deliberately does NOT ask, so that no reader takes
        // "the whole shape" for more than it is: the entry-name and
        // containing-type clauses (`ValidateMapEntry`:8460-8461) are A22's
        // reference check and are refused as `DIAGNOSTIC_MAP_ENTRY_TYPE`, the
        // value-enum-key clause is `D-33`'s recorded class, and the extension
        // members are refused by their PRESENCE alone (the declaration's own
        // numbers are the reference compiler's business, contract clause 12).
        // Field 5 is modelled since `W-45` - the extension-range rules need
        // its numbers - while field 6 stays a count, so presence is all this
        // shape needs from either (ledger `D-62`).
        const FieldDescriptorProto* key = 0;
        const FieldDescriptorProto* value = 0;
        for (std::size_t i = 0; i < message.field.size(); ++i) {
            if (message.field[i].number == 1) key = &message.field[i];
            if (message.field[i].number == 2) value = &message.field[i];
        }
        if (key == 0 || value == 0) {
            return fail_malformed_map_entry(file, qualified_name, error);
        }
        if (message.field.size() != 2) {
            return fail_map_entry_shape(file, qualified_name,
                                        "exactly two fields", error);
        }
        // W-37: protoc locates the pair by POSITION, not by number
        // (`ValidateMapEntry`: `message->map_key()` is `field(0)` and
        // `message->map_value()` is `field(1)`, descriptor.cc:8462-8471), so a
        // descriptor that declares field 2 first and field 1 second is not a
        // map entry however the numbers look. The numbers are still required,
        // because the entry's own shape says the key is 1 and the value is 2;
        // what this adds is that the declaration order must agree with the
        // numbers, which is the only shape protoc's positional read accepts.
        if (message.field[0].number != 1 || message.field[1].number != 2) {
            return fail_map_entry_shape(
                file, qualified_name,
                "an order that declares the field numbered 1 first and the "
                "field numbered 2 second",
                error);
        }
        // An absent label is read as LABEL_OPTIONAL, because that is protoc's
        // own default for the field (`proto.label()`), so only a label the
        // descriptor actually declared can disagree here.
        if ((key->has_label &&
             key->label != FieldDescriptorProto::LABEL_OPTIONAL) ||
            key->name != "key" ||
            !valid_map_key_type(key->type)) {
            return fail_map_entry_shape(
                file, qualified_name,
                "a key field named \"key\" numbered 1, LABEL_OPTIONAL, with a "
                "legal key type",
                error);
        }
        if ((value->has_label &&
             value->label != FieldDescriptorProto::LABEL_OPTIONAL) ||
            value->name != "value") {
            return fail_map_entry_shape(
                file, qualified_name,
                "a value field named \"value\" numbered 2, LABEL_OPTIONAL",
                error);
        }
        if (!message.nested_type.empty() || !message.enum_type.empty()) {
            return fail_map_entry_shape(file, qualified_name,
                                        "no nested messages or enums", error);
        }
        if (!message.extension_range.empty() ||
            message.extension_count != 0) {
            return fail_map_entry_shape(file, qualified_name,
                                        "no extension ranges or extension "
                                        "fields",
                                        error);
        }
    }
    for (std::size_t i = 0; i < message.nested_type.size(); ++i) {
        if (!validate_message_map_entries(
                file, message.nested_type[i],
                joined_name(qualified_name, message.nested_type[i].name),
                error)) {
            return false;
        }
    }
    return true;
}

// W-42's port of protoc's two helpers: `EnumValueToPascalCase`
// (descriptor.cc:853-872) and `PrefixRemover` (:875-935). The folded key is
// what makes `FOO`/`foo` and `E_FOO`/`FOO` collide even when the exact-name
// rule and the number rule would not.
std::string enum_value_to_pascal_case(const std::string& input)
{
    bool next_upper = true;
    std::string result;
    result.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        const char character = input[i];
        if (character == '_') {
            next_upper = true;
        } else {
            if (next_upper && character >= 'a' && character <= 'z') {
                result.push_back(static_cast<char>(character - 'a' + 'A'));
            } else if (!next_upper && character >= 'A' &&
                       character <= 'Z') {
                result.push_back(static_cast<char>(character - 'A' + 'a'));
            } else {
                result.push_back(character);
            }
            next_upper = false;
        }
    }
    return result;
}

struct EnumPrefixRemover
{
    explicit EnumPrefixRemover(const std::string& prefix)
    {
        for (std::size_t i = 0; i < prefix.size(); ++i) {
            const char character = prefix[i];
            if (character == '_') continue;
            prefix_.push_back(character >= 'A' && character <= 'Z'
                                  ? static_cast<char>(character - 'A' + 'a')
                                  : character);
        }
    }

    std::string maybe_remove(const std::string& input) const
    {
        std::size_t i = 0;
        std::size_t j = 0;
        for (; i < input.size() && j < prefix_.size(); ++i) {
            if (input[i] == '_') continue;
            const char lower = input[i] >= 'A' && input[i] <= 'Z'
                                   ? static_cast<char>(input[i] - 'A' + 'a')
                                   : input[i];
            if (lower != prefix_[j++]) return input;
        }
        if (j < prefix_.size()) return input;
        while (i < input.size() && input[i] == '_') ++i;
        if (i == input.size()) return input;
        return input.substr(i);
    }

private:
    std::string prefix_;
};

// D-28 (fix round r13, contract clause 12): every enum's value numbers, over
// every available file, before anything is resolved and in both linking modes -
// A26's reference-free precedent, one sweep beside the entry-shape sweep,
// because no resolution pass walks a top-level enum's values and an enum
// nothing references is still descriptor input.
//
// Two questions about the same entity, both answerable from the descriptor
// alone: the enum must DECLARE a value (D-32, fix round r16), and two values
// may share a number only when the enum declares `option allow_alias = true`
// (D-28). The second is protoc's own rule when it builds a descriptor, and the
// source path refuses the same shape as text (`proto_parser.cpp`'s
// duplicate-number rule). The option travels in the model (`EnumOptions`) since
// fix round r13, which is what lets an intentional alias -
// `tests/codegen/enums/data/enum_codegen.proto`'s `STARTED = 1; ACTIVE = 1;` -
// link, so refusing an aliased enum would be the same divergence D-28 closed,
// in the other direction. A third question rides the same sweep since S5:
// a proto3 enum's FIRST value must be zero (`W-44`), which the source path
// refuses as text and protoc refuses while it builds the file.
bool validate_enum_value_numbers_in_enum(const SchemaFile& file,
                                         const EnumDescriptorProto& enumeration,
                                         const std::string& enum_name,
                                         const DescriptorPath& enum_path,
                                         Diagnostic& error)
{
    // The empty list is the rule's subject, not the option's: protoc refuses an
    // enum with no value even when the enum writes `option allow_alias = true`
    // (`Enums must contain at least one value.`, measured this round on
    // `enum-empty-aliased`), so this question is asked BEFORE the option's
    // early return below. The parser refuses the source twin as text, so the
    // shape is descriptor input only - a hand-built or mutated `.pbs`, which is
    // what this mode exists for; an enum nothing references is still descriptor
    // input, so the walk does not need a reference (A26's precedent).
    if (enumeration.value.empty()) {
        return fail_at_code(
            file,
            "enum \"" + enum_name +
                "\" contains no values; an enum must contain at least one "
                "value",
            declaration_location(file, enum_path), enum_path,
            DIAGNOSTIC_EMPTY_ENUM, error);
    }
    // W-44: a proto3 enum must declare zero as its FIRST value, and the rule
    // is asked before the option's early return below because protoc asks it
    // before its alias branch too (`CheckEnumValueUniqueness`,
    // descriptor.cc:8245: `The first enum value must be zero for open enums.`).
    // The parser refuses the source twin with the same sentence this message
    // carries (`proto_parser.cpp`'s enum rule, "the first proto3 enum value
    // must be zero"), so the two frontends agree on the rule. Only a proto3
    // file is asked: proto2 gives the first value no special meaning.
    if (classify_file_syntax(file.file) == FILE_SYNTAX_PROTO3 &&
        enumeration.value[0].number != 0) {
        const DescriptorPath value_path = enum_value_path(enum_path, 0);
        return fail_at_code(
            file,
            "enum \"" + enum_name + "\" declares \"" +
                view_text(enumeration.value[0].name) + "\" = " +
                number_text(enumeration.value[0].number) +
                " as its first value; the first proto3 enum value must be zero",
            declaration_location(file, value_path), value_path,
            DIAGNOSTIC_PROTO3_ENUM_FIRST_VALUE, error);
    }
    // W-42: protoc compares enum value names after stripping the enum-name
    // prefix and PascalCasing the remainder (`CheckEnumValueUniqueness`,
    // descriptor.cc:6957-7021). The check runs before the allow_alias early
    // return because the option licenses same-number aliases, not two
    // different names that fold together: `E_FOO = 0; FOO = 1;` is refused
    // even with allow_alias. A pair that maps to the same number is skipped
    // here and handled by the number rule below (or accepted as an alias);
    // a pair with the identical name is skipped for the exact-name rule.
    const std::string enum_prefix = view_text(enumeration.name);
    const EnumPrefixRemover remover(enum_prefix);
    std::map<std::string, std::size_t> folded_names;
    for (std::size_t i = 0; i < enumeration.value.size(); ++i) {
        const str_view& value_name = enumeration.value[i].name;
        const std::string folded = enum_value_to_pascal_case(
            remover.maybe_remove(view_text(value_name)));
        const std::pair<std::map<std::string, std::size_t>::iterator, bool>
            inserted = folded_names.insert(
                std::map<std::string, std::size_t>::value_type(folded, i));
        if (inserted.second) continue;
        const EnumValueDescriptorProto& earlier =
            enumeration.value[inserted.first->second];
        if (earlier.name == value_name || earlier.number == enumeration.value[i].number) {
            continue;
        }
        const bool allow_alias =
            enumeration.options.has_allow_alias &&
            enumeration.options.allow_alias;
        std::string message =
            "Enum name " + view_text(value_name) + " has the same name as " +
            view_text(earlier.name) +
            " if you ignore case and strip out the enum name prefix (if any).";
        if (allow_alias) {
            message +=
                " (If you are using allow_alias, please assign the same "
                "number to each enum value name.)";
        }
        const DescriptorPath value_path =
            enum_value_path(enum_path, static_cast<int>(i));
        return fail_at_code(
            file, message, declaration_location(file, value_path),
            value_path, DIAGNOSTIC_DUPLICATE_SYMBOL, error);
    }
    if (enumeration.options.has_allow_alias &&
        enumeration.options.allow_alias) {
        return true;
    }
    std::map<std::int32_t, str_view> first_by_number;
    for (std::size_t i = 0; i < enumeration.value.size(); ++i) {
        const std::int32_t number = enumeration.value[i].number;
        const str_view& value_name = enumeration.value[i].name;
        const std::pair<std::map<std::int32_t, str_view>::iterator, bool> inserted =
            first_by_number.insert(
                std::map<std::int32_t, str_view>::value_type(number, value_name));
        if (!inserted.second) {
            const DescriptorPath value_path =
                enum_value_path(enum_path, static_cast<int>(i));
            return fail_at_code(
                file,
                "enum value \"" + view_text(value_name) + "\" of \"" + enum_name +
                    "\" repeats number " + number_text(number) +
                    ", which \"" + view_text(inserted.first->second) +
                    "\" already uses; set option allow_alias = true to allow "
                    "aliases",
                declaration_location(file, value_path), value_path,
                DIAGNOSTIC_DUPLICATE_ENUM_NUMBER, error);
        }
    }
    return true;
}

// The walk: a message's own enums first, then its nested types, in declaration
// order - the same order validate_message_map_entries uses, so the first
// malformed entity a reader would meet is the one reported.
bool validate_message_enum_value_numbers(const SchemaFile& file,
                                         const DescriptorProto& message,
                                         const std::string& qualified_name,
                                         const DescriptorPath& message_path,
                                         Diagnostic& error)
{
    for (std::size_t i = 0; i < message.enum_type.size(); ++i) {
        if (!validate_enum_value_numbers_in_enum(
                file, message.enum_type[i],
                joined_name(qualified_name, message.enum_type[i].name),
                message_enum_path(message_path, static_cast<int>(i)), error)) {
            return false;
        }
    }
    for (std::size_t i = 0; i < message.nested_type.size(); ++i) {
        if (!validate_message_enum_value_numbers(
                file, message.nested_type[i],
                joined_name(qualified_name, message.nested_type[i].name),
                message_nested_path(message_path, static_cast<int>(i)),
                error)) {
            return false;
        }
    }
    return true;
}

// W-45: the extension ranges a descriptor declares, over the descriptor alone.
// Four shapes protoc refuses when it builds a file share one code, because
// they are one category - the declaration does not describe usable extension
// numbers:
//
//   * a range in a proto3 file (`Extension ranges are not allowed in proto3.`,
//     descriptor.cc:7898);
//   * a non-positive start (`BuildExtensionRange`, :6884-6890, "Extension
//     numbers must be positive integers.");
//   * an exclusive end that does not exceed its start (`:6892-6896`);
//   * a range that includes a declared field number (:6426-6430), and two
//     overlapping ranges (:6454-6482).
//
// The source path refuses every one of them as text before it reaches the
// model (`proto_parser.cpp`'s `parse_extensions`, `add_range` and
// `validate_message_constraints`), so this is the descriptor-origin half of
// the same frontend parity rule. The walk is the declaration order the other
// reference-free sweeps use, so the first offending range is the one reported.
bool validate_message_extension_ranges(const SchemaFile& file,
                                       const DescriptorProto& message,
                                       const std::string& qualified_name,
                                       const DescriptorPath& message_path,
                                       bool is_proto3, Diagnostic& error)
{
    for (std::size_t i = 0; i < message.extension_range.size(); ++i) {
        const DescriptorProto_ExtensionRange& range = message.extension_range[i];
        const DescriptorPath range_path =
            message_extension_range_path(message_path, static_cast<int>(i));
        const std::string bounds =
            "[" + number_text(range.start) + ", " + number_text(range.end) +
            ")";
        if (is_proto3) {
            // protoc reports one error for the message, at its first range,
            // because a proto3 file cannot carry extension ranges at all.
            return fail_at_code(
                file,
                "message \"" + qualified_name +
                    "\" declares extension range " + bounds +
                    "; extension ranges are allowed only in proto2",
                declaration_location(file, range_path), range_path,
                DIAGNOSTIC_INVALID_EXTENSION_RANGE, error);
        }
        if (range.start <= 0) {
            return fail_at_code(
                file,
                "message \"" + qualified_name + "\" declares extension range " +
                    bounds +
                    " whose start is not positive; extension numbers must be "
                    "positive integers",
                declaration_location(file, range_path), range_path,
                DIAGNOSTIC_INVALID_EXTENSION_RANGE, error);
        }
        if (range.end <= range.start) {
            return fail_at_code(
                file,
                "message \"" + qualified_name + "\" declares extension range " +
                    bounds +
                    " whose end is not greater than its start",
                declaration_location(file, range_path), range_path,
                DIAGNOSTIC_INVALID_EXTENSION_RANGE, error);
        }
        // CR-07: the exclusive end may not exceed the protocol maximum plus
        // one. protoc states the same bound in `ValidateExtensionRangeOptions`
        // (`descriptor.cc:8395-8404`, "Extension numbers cannot be greater than
        // 536870911.") and applies it to every message that does NOT declare
        // MessageSet wire format: a MessageSet's extension numbers are plain
        // int32s, so its ranges may exceed `FieldDescriptor::kMaxNumber`
        // (`max_extension_range`, `:8385-8389`). The bound is checked here,
        // beside the other range-shape rules, because a `.pbs` range is the
        // only door through which an out-of-protocol extension contract can
        // reach the linker - the source parser bounds a range endpoint at
        // 536870911 as text.
        if (!message.options.message_set_wire_format &&
            range.end > kMaxExtensionEnd) {
            return fail_at_code(
                file,
                "message \"" + qualified_name + "\" declares extension range " +
                    bounds + " whose exclusive end exceeds " +
                    number_text(kMaxExtensionEnd) +
                    "; extension numbers cannot be greater than " +
                    number_text(kMaxExtensionEnd - 1),
                declaration_location(file, range_path), range_path,
                DIAGNOSTIC_INVALID_EXTENSION_RANGE, error);
        }
        for (std::size_t j = 0; j < message.field.size(); ++j) {
            const FieldDescriptorProto& field = message.field[j];
            if (field.number < range.start || field.number >= range.end) {
                continue;
            }
            return fail_at_code(
                file,
                "extension range " + bounds + " in message \"" +
                    qualified_name + "\" includes field \"" +
                    view_text(field.name) + "\" (" + number_text(field.number) +
                    ")",
                declaration_location(file, range_path), range_path,
                DIAGNOSTIC_INVALID_EXTENSION_RANGE, error);
        }
        for (std::size_t j = 0; j < i; ++j) {
            const DescriptorProto_ExtensionRange& earlier =
                message.extension_range[j];
            if (range.end <= earlier.start || earlier.end <= range.start) {
                continue;
            }
            return fail_at_code(
                file,
                "extension range " + bounds + " in message \"" +
                    qualified_name + "\" overlaps range [" +
                    number_text(earlier.start) + ", " +
                    number_text(earlier.end) + ")",
                declaration_location(file, range_path), range_path,
                DIAGNOSTIC_INVALID_EXTENSION_RANGE, error);
        }
    }
    for (std::size_t i = 0; i < message.nested_type.size(); ++i) {
        if (!validate_message_extension_ranges(
                file, message.nested_type[i],
                joined_name(qualified_name, message.nested_type[i].name),
                message_nested_path(message_path, static_cast<int>(i)),
                is_proto3, error)) {
            return false;
        }
    }
    return true;
}


// CR-06, the enum half: the reserved declarations of one enum, in the order
// protoc's `BuildEnum` asks its questions (descriptor.cc:7060-7125). An enum
// reserved range is INCLUSIVE at both ends, so the overlap test is inclusive
// too (`:7076-7092`) - unlike the message twin, where the end is exclusive.
bool validate_enum_reserved(const SchemaFile& file,
                            const EnumDescriptorProto& enumeration,
                            const std::string& qualified_name,
                            const DescriptorPath& enum_path,
                            Diagnostic& error)
{
    for (std::size_t i = 0; i < enumeration.reserved_range.size(); ++i) {
        const EnumDescriptorProto_EnumReservedRange& range =
            enumeration.reserved_range[i];
        const DescriptorPath range_path =
            enum_reserved_range_path(enum_path, static_cast<int>(i));
        const std::string bounds = "[" + number_text(range.start) + ", " +
                                   number_text(range.end) + "]";
        if (range.start > range.end) {
            return fail_at_code(
                file,
                "enum \"" + qualified_name + "\" declares reserved range " +
                    bounds + " whose end is less than its start",
                declaration_location(file, range_path), range_path,
                DIAGNOSTIC_INVALID_RESERVED_RANGE, error);
        }
        for (std::size_t j = 0; j < i; ++j) {
            const EnumDescriptorProto_EnumReservedRange& earlier =
                enumeration.reserved_range[j];
            if (range.start > earlier.end || earlier.start > range.end) {
                continue;
            }
            return fail_at_code(
                file,
                "reserved range " + bounds + " in enum \"" + qualified_name +
                    "\" overlaps range [" + number_text(earlier.start) + ", " +
                    number_text(earlier.end) + "]",
                declaration_location(file, range_path), range_path,
                DIAGNOSTIC_INVALID_RESERVED_RANGE, error);
        }
    }

    std::set<str_view> reserved_names;
    for (std::size_t i = 0; i < enumeration.reserved_name.size(); ++i) {
        if (reserved_names.insert(enumeration.reserved_name[i]).second) {
            continue;
        }
        const DescriptorPath name_path =
            enum_reserved_name_path(enum_path, static_cast<int>(i));
        return fail_at_code(
            file,
            "enum value name \"" + view_text(enumeration.reserved_name[i]) +
                "\" is reserved multiple times in enum \"" + qualified_name +
                "\"",
            declaration_location(file, name_path), name_path,
            DIAGNOSTIC_INVALID_RESERVED_RANGE, error);
    }

    for (std::size_t i = 0; i < enumeration.value.size(); ++i) {
        const EnumValueDescriptorProto& value = enumeration.value[i];
        const DescriptorPath value_path =
            enum_value_path(enum_path, static_cast<int>(i));
        for (std::size_t j = 0; j < enumeration.reserved_range.size(); ++j) {
            const EnumDescriptorProto_EnumReservedRange& range =
                enumeration.reserved_range[j];
            if (value.number < range.start || value.number > range.end) continue;
            const DescriptorPath range_path =
                enum_reserved_range_path(enum_path, static_cast<int>(j));
            return fail_at_code(
                file,
                "enum value \"" + view_text(value.name) + "\" (" +
                    number_text(value.number) +
                    ") uses a number reserved by [" +
                    number_text(range.start) + ", " + number_text(range.end) +
                    "] in enum \"" + qualified_name + "\"",
                declaration_location(file, range_path), range_path,
                DIAGNOSTIC_INVALID_RESERVED_RANGE, error);
        }
        if (reserved_names.find(value.name) != reserved_names.end()) {
            return fail_at_code(
                file,
                "enum value name \"" + view_text(value.name) +
                    "\" is reserved in enum \"" + qualified_name + "\"",
                declaration_location(file, value_path), value_path,
                DIAGNOSTIC_INVALID_RESERVED_RANGE, error);
        }
    }
    return true;
}


// CR-06, the message half: the reserved declarations of one message and of
// every message nested in it. The five message questions are protoc's own
// (descriptor.cc:6396-6480): range validity, range overlap, a repeated
// reserved name, a field using a reserved number or name, and an extension
// range overlapping a reserved range. A reserved range here is [start, end):
// `start` inclusive, `end` exclusive (descriptor.proto:168-174).
bool validate_message_reserved(const SchemaFile& file,
                               const DescriptorProto& message,
                               const std::string& qualified_name,
                               const DescriptorPath& message_path,
                               Diagnostic& error)
{
    for (std::size_t i = 0; i < message.reserved_range.size(); ++i) {
        const DescriptorProto_ReservedRange& range = message.reserved_range[i];
        const DescriptorPath range_path =
            message_reserved_range_path(message_path, static_cast<int>(i));
        const std::string bounds = "[" + number_text(range.start) + ", " +
                                   number_text(range.end) + ")";
        if (range.start <= 0) {
            return fail_at_code(
                file,
                "message \"" + qualified_name + "\" declares reserved range " +
                    bounds +
                    " whose start is not positive; reserved numbers must be "
                    "positive integers",
                declaration_location(file, range_path), range_path,
                DIAGNOSTIC_INVALID_RESERVED_RANGE, error);
        }
        if (range.start >= range.end) {
            return fail_at_code(
                file,
                "message \"" + qualified_name + "\" declares reserved range " +
                    bounds + " whose end is not greater than its start",
                declaration_location(file, range_path), range_path,
                DIAGNOSTIC_INVALID_RESERVED_RANGE, error);
        }
        for (std::size_t j = 0; j < i; ++j) {
            const DescriptorProto_ReservedRange& earlier =
                message.reserved_range[j];
            if (range.end <= earlier.start || earlier.end <= range.start) {
                continue;
            }
            return fail_at_code(
                file,
                "reserved range " + bounds + " in message \"" +
                    qualified_name + "\" overlaps range [" +
                    number_text(earlier.start) + ", " +
                    number_text(earlier.end) + ")",
                declaration_location(file, range_path), range_path,
                DIAGNOSTIC_INVALID_RESERVED_RANGE, error);
        }
    }

    std::set<str_view> reserved_names;
    for (std::size_t i = 0; i < message.reserved_name.size(); ++i) {
        if (reserved_names.insert(message.reserved_name[i]).second) continue;
        const DescriptorPath name_path =
            message_reserved_name_path(message_path, static_cast<int>(i));
        return fail_at_code(
            file,
            "field name \"" + view_text(message.reserved_name[i]) +
                "\" is reserved multiple times in message \"" + qualified_name +
                "\"",
            declaration_location(file, name_path), name_path,
            DIAGNOSTIC_INVALID_RESERVED_RANGE, error);
    }

    for (std::size_t i = 0; i < message.field.size(); ++i) {
        const FieldDescriptorProto& field = message.field[i];
        for (std::size_t j = 0; j < message.reserved_range.size(); ++j) {
            const DescriptorProto_ReservedRange& range = message.reserved_range[j];
            if (field.number < range.start || field.number >= range.end) continue;
            const DescriptorPath range_path =
                message_reserved_range_path(message_path, static_cast<int>(j));
            return fail_at_code(
                file,
                "field \"" + view_text(field.name) + "\" (" +
                    number_text(field.number) +
                    ") uses a number reserved by [" + number_text(range.start) +
                    ", " + number_text(range.end) + ") in message \"" +
                    qualified_name + "\"",
                declaration_location(file, range_path), range_path,
                DIAGNOSTIC_INVALID_RESERVED_RANGE, error);
        }
        if (reserved_names.find(field.name) != reserved_names.end()) {
            const DescriptorPath field_path =
                message_field_path(message_path, static_cast<int>(i));
            return fail_at_code(
                file,
                "field name \"" + view_text(field.name) +
                    "\" is reserved in message \"" + qualified_name + "\"",
                declaration_location(file, field_path), field_path,
                DIAGNOSTIC_INVALID_RESERVED_RANGE, error);
        }
    }

    for (std::size_t i = 0; i < message.extension_range.size(); ++i) {
        const DescriptorProto_ExtensionRange& extension =
            message.extension_range[i];
        const DescriptorPath extension_path =
            message_extension_range_path(message_path, static_cast<int>(i));
        for (std::size_t j = 0; j < message.reserved_range.size(); ++j) {
            const DescriptorProto_ReservedRange& range = message.reserved_range[j];
            if (extension.start >= range.end || range.start >= extension.end) {
                continue;
            }
            return fail_at_code(
                file,
                "extension range [" + number_text(extension.start) + ", " +
                    number_text(extension.end) + ") in message \"" +
                    qualified_name + "\" overlaps reserved range [" +
                    number_text(range.start) + ", " + number_text(range.end) +
                    ")",
                declaration_location(file, extension_path), extension_path,
                DIAGNOSTIC_INVALID_RESERVED_RANGE, error);
        }
    }

    for (std::size_t i = 0; i < message.nested_type.size(); ++i) {
        if (!validate_message_reserved(
                file, message.nested_type[i],
                joined_name(qualified_name, message.nested_type[i].name),
                message_nested_path(message_path, static_cast<int>(i)),
                error)) {
            return false;
        }
    }
    return true;
}


// CR-23: a message that declares MessageSet wire format must not declare
// ordinary fields. The rule is protoc's (`BuildField`, descriptor.cc:6372-6380;
// the same check at `:7994-8001`), and it is about the message's wire encoding,
// not about a reference, so it rides the reference-free stage. A MessageSet
// whose only members are extensions - or that declares no members at all -
// stays accepted, exactly as protoc accepts it.
bool validate_message_set_in_message(const SchemaFile& file,
                                     const DescriptorProto& message,
                                     const std::string& qualified_name,
                                     const DescriptorPath& message_path,
                                     Diagnostic& error)
{
    if (message.options.message_set_wire_format && !message.field.empty()) {
        const DescriptorPath options_path = message_options_path(message_path);
        return fail_at_code(
            file,
            "message \"" + qualified_name +
                "\" declares message_set_wire_format and " +
                number_text(static_cast<std::int32_t>(message.field.size())) +
                " field(s); a MessageSet cannot have fields, only extensions",
            declaration_location(file, options_path), options_path,
            DIAGNOSTIC_MESSAGE_SET_WITH_FIELDS, error);
    }
    for (std::size_t i = 0; i < message.nested_type.size(); ++i) {
        if (!validate_message_set_in_message(
                file, message.nested_type[i],
                joined_name(qualified_name, message.nested_type[i].name),
                message_nested_path(message_path, static_cast<int>(i)),
                error)) {
            return false;
        }
    }
    return true;
}


// The text for a reference that failed because the name is known but must not
// be used as a type here. Each category states the rule instead of copying
// protoc's wording: its texts are editions-era UX paragraphs, and the
// differential checkpoint compares verdicts, not message texts (Appendix C).
std::string reference_failure_message(const SchemaFile& file,
                                      const SymbolIndex& symbols,
                                      const std::string& raw,
                                      ResolveOutcome outcome,
                                      const std::string& matched)
{
    const std::string resolved = matched.empty() ? raw : matched;
    if (outcome == RESOLVE_MAP_ENTRY) {
        return "map entry type \"" + resolved + "\" is not a user type: only the "
               "map field that owns it may reference it; declare the field as "
               "map<KeyType, ValueType> instead";
    }
    if (outcome == RESOLVE_INVISIBLE) {
        const std::string defining = defining_file_name(symbols, resolved);
        std::string message = "type \"" + resolved + "\"";
        if (defining.empty()) {
            message += " is not visible from \"" + schema_file_label(file) + "\"";
        } else {
            message += " is defined in \"" + defining + "\", which is not visible "
                       "from \"" + schema_file_label(file) + "\"";
        }
        message += "; add the import of that file";
        return message;
    }
    const std::map<std::string, SymbolEntry>::const_iterator found =
        symbols.symbols.find(resolved);
    std::string category = "a declaration";
    if (found != symbols.symbols.end()) {
        category = declaration_category(*found->second.owner, found->second.path,
                                        found->second.kind);
    }
    return "type reference \"" + raw + "\" is not a type: it is " + category +
           " (\"" + resolved + "\")";
}

// The text for a reference the index cannot resolve at all. A committed
// aggregate whose member path is absent names the resolved compound path, so
// the shadowing case does not read like a plain typo (Step 4.3, oracle B1).
std::string missing_reference_message(const std::string& raw,
                                      const std::string& scope,
                                      const std::string& matched)
{
    if (matched.empty()) return "unresolved type " + raw + " in " + scope;
    return "unresolved type " + raw + " in " + scope + ": it is resolved to \"" +
           matched + "\", which is not defined";
}

// The one sentence the cross-syntax enum rule records (Step 6.1, R11). It
// states the rule instead of copying protoc's editions-era wording ("is not an
// open enum"), which is reference evidence and not a template (Appendix C C2):
// the text names the enum, the file that declares it, the requesting file, and
// the rule itself, so no reader has to know about editions. Openness belongs
// to the file that declares the enum, which is why the declaring file's syntax
// is the whole explanation.
std::string cross_syntax_enum_message(const SchemaFile& requesting,
                                      const SchemaFile& declaring,
                                      const std::string& enum_name)
{
    return "enum type \"" + enum_name + "\" is declared in \"" +
           view_text(declaring.file.name) +
           "\", a proto2 file, and must not be used by a proto3 field in \"" +
           view_text(requesting.file.name) +
           "\": a proto3 field requires an enum declared in a proto3 file";
}

// The one sentence the compatibility pass records for a permitted external
// reference (A5). It names the type and states exactly what the mode could not
// validate, so a reader of the warning sees the warranty's edge. It is
// separate from a refusal's message on purpose: this is a warning about
// missing external information, not a verdict.
std::string external_reference_message(const std::string& type_name)
{
    return "external type \"" + type_name +
           "\" is not declared in the linked set: its members, defaults, and "
           "dependency visibility were not validated";
}

// True when the character is an ASCII hexadecimal digit (the C-escape grammar
// below is ASCII-only, exactly as `absl::CUnescape` and the parser are).
bool is_hex_digit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

// True when `text` is a C-escape sequence `absl::CUnescape` accepts, which is
// the grammar a descriptor-origin bytes default must carry: protoc applies
// CUnescape to `default_value` while it builds a bytes field and reports
// `Invalid escaping in default value.` when it fails (descriptor.cc:6728-6740),
// and the value then reaches the generator verbatim (ledger `D-58`).
//
// The accepted set was measured against libprotoc 30.1 through
// `--descriptor_set_in` on the crafted strings in
// `build/plan4/fix-r25/probe/escape_probe{,2}.py`: the simple escapes
// `a b f n r t v \ \' " ?`, an octal escape of one to three digits whose
// value is at most 255, a hex escape of one or more digits whose value is at
// most 255, and `\u`/`\U` of exactly four/eight hex digits naming a
// non-surrogate code point at most U+10FFFF. Everything else - an unknown
// letter, a bare `\x`, a trailing backslash - is refused, which is the source
// parser's own rule too (codegen/parser/proto_parser.cpp:371-433), so the two
// frontends agree on the shape instead of diverging on it.
bool valid_bytes_default_escape(const std::string& text)
{
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\\') continue;
        if (i + 1 == text.size()) return false;
        const char escaped = text[++i];
        if (escaped == 'a' || escaped == 'b' || escaped == 'f' ||
            escaped == 'n' || escaped == 'r' || escaped == 't' ||
            escaped == 'v' || escaped == '\\' || escaped == '\'' ||
            escaped == '"' || escaped == '?') {
            continue;
        }
        if (escaped == 'x' || escaped == 'X') {
            std::uint32_t value = 0;
            std::size_t digits = 0;
            while (i + 1 < text.size() && is_hex_digit(text[i + 1])) {
                const char c = text[++i];
                value = value * 16u +
                        (c <= '9' ? static_cast<std::uint32_t>(c - '0')
                                  : static_cast<std::uint32_t>((c | 0x20) - 'a' + 10));
                ++digits;
                if (value > 255u) return false;
            }
            if (digits == 0) return false;
            continue;
        }
        if (escaped == 'u' || escaped == 'U') {
            const std::size_t digits = escaped == 'u' ? 4u : 8u;
            std::uint32_t value = 0;
            for (std::size_t d = 0; d < digits; ++d) {
                if (i + 1 == text.size() || !is_hex_digit(text[i + 1])) {
                    return false;
                }
                const char c = text[++i];
                value = value * 16u +
                        (c <= '9' ? static_cast<std::uint32_t>(c - '0')
                                  : static_cast<std::uint32_t>((c | 0x20) - 'a' + 10));
            }
            if (value > 0x10FFFFu || (value >= 0xD800u && value < 0xE000u)) {
                return false;
            }
            continue;
        }
        if (escaped >= '0' && escaped <= '7') {
            std::uint32_t value = static_cast<std::uint32_t>(escaped - '0');
            std::size_t digits = 1;
            while (digits < 3 && i + 1 < text.size() &&
                   text[i + 1] >= '0' && text[i + 1] <= '7') {
                value = value * 8u + static_cast<std::uint32_t>(text[++i] - '0');
                ++digits;
            }
            if (value > 255u) return false;
            continue;
        }
        return false;
    }
    return true;
}

} // namespace

// The decoder that belongs to the grammar above: it turns the C-escaped text
// a bytes default carries into the bytes it denotes, with exactly the escapes
// `valid_bytes_default_escape` accepts. The reference compiler does the same
// with `absl::CUnescape` while it builds a bytes field
// (descriptor.cc:6728-6741), and this project's own parser does it with the
// tokenizer's own escape handling (proto_parser.cpp:380-435) - so the two
// frontends and the emitter agree on what a bytes default means.
//
// The decode is byte-exact: a `\u`/`\U` escape becomes the UTF-8 encoding of
// the code point (the bytes a protobuf string literal holds), and an octal or
// hex escape becomes the byte it names.
bool decode_bytes_escape(const std::string& text, std::string& result)
{
    std::string decoded;
    decoded.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\\') {
            decoded.push_back(text[i]);
            continue;
        }
        if (i + 1 == text.size()) return false;
        const char escaped = text[++i];
        switch (escaped) {
            case 'a': decoded.push_back('\a'); break;
            case 'b': decoded.push_back('\b'); break;
            case 'f': decoded.push_back('\f'); break;
            case 'n': decoded.push_back('\n'); break;
            case 'r': decoded.push_back('\r'); break;
            case 't': decoded.push_back('\t'); break;
            case 'v': decoded.push_back('\v'); break;
            case '\\': decoded.push_back('\\'); break;
            case '\'': decoded.push_back('\''); break;
            case '"': decoded.push_back('"'); break;
            case '?': decoded.push_back('?'); break;
            case 'x':
            case 'X': {
                std::uint32_t value = 0;
                std::size_t digits = 0;
                while (i + 1 < text.size() && is_hex_digit(text[i + 1])) {
                    const char c = text[++i];
                    value = value * 16u +
                            (c <= '9' ? static_cast<std::uint32_t>(c - '0')
                                      : static_cast<std::uint32_t>((c | 0x20) - 'a' + 10));
                    ++digits;
                    if (value > 255u) return false;
                }
                if (digits == 0) return false;
                decoded.push_back(static_cast<char>(value));
                break;
            }
            case 'u':
            case 'U': {
                const std::size_t digits = escaped == 'u' ? 4u : 8u;
                std::uint32_t value = 0;
                for (std::size_t d = 0; d < digits; ++d) {
                    if (i + 1 >= text.size() || !is_hex_digit(text[i + 1])) {
                        return false;
                    }
                    const char c = text[++i];
                    value = value * 16u +
                            (c <= '9' ? static_cast<std::uint32_t>(c - '0')
                                      : static_cast<std::uint32_t>((c | 0x20) - 'a' + 10));
                }
                if (value > 0x10FFFFu || (value >= 0xD800u && value < 0xE000u)) {
                    return false;
                }
                // The UTF-8 encoding of the code point, the same bytes the
                // parser's own escape handling produces.
                if (value <= 0x7Fu) {
                    decoded.push_back(static_cast<char>(value));
                } else if (value <= 0x7FFu) {
                    decoded.push_back(static_cast<char>(0xC0u | (value >> 6)));
                    decoded.push_back(static_cast<char>(0x80u | (value & 0x3Fu)));
                } else if (value <= 0xFFFFu) {
                    decoded.push_back(static_cast<char>(0xE0u | (value >> 12)));
                    decoded.push_back(static_cast<char>(0x80u | ((value >> 6) & 0x3Fu)));
                    decoded.push_back(static_cast<char>(0x80u | (value & 0x3Fu)));
                } else {
                    decoded.push_back(static_cast<char>(0xF0u | (value >> 18)));
                    decoded.push_back(static_cast<char>(0x80u | ((value >> 12) & 0x3Fu)));
                    decoded.push_back(static_cast<char>(0x80u | ((value >> 6) & 0x3Fu)));
                    decoded.push_back(static_cast<char>(0x80u | (value & 0x3Fu)));
                }
                break;
            }
            default:
                if (escaped < '0' || escaped > '7') return false;
                {
                    std::uint32_t value = static_cast<std::uint32_t>(escaped - '0');
                    std::size_t digits = 1;
                    while (digits < 3 && i + 1 < text.size() &&
                           text[i + 1] >= '0' && text[i + 1] <= '7') {
                        value = value * 8u + static_cast<std::uint32_t>(text[++i] - '0');
                        ++digits;
                    }
                    if (value > 255u) return false;
                    decoded.push_back(static_cast<char>(value));
                }
                break;
        }
    }
    result.swap(decoded);
    return true;
}

// The rules this seam declares, defined here in `namespace internal` rather
// than in the anonymous block above: owns_map_entry and validate_default. The
// three message-level validators stay anonymous and are reached through the
// forwarders at the end of this file, after the anonymous block closes. The
// definitions stay in this translation unit, so declaration and definition
// cannot drift.
namespace internal {

// Ownership rule for a synthetic map entry, asked after the caller has proved
// the resolved symbol is a message and the message carries the map_entry
// marker. The marker is checked again here because it is this rule's
// PRECONDITION, not a courtesy of the one caller (W-49): a real nested message
// whose name, nesting and label happen to match must never be claimed as some
// field's entry.
//
// The remaining clauses are protoc's own (A22, measured): the entry is nested
// directly in the field's own message, the field is repeated, and the field's
// NAME derives the entry's simple name through the shared map_entry_name
// (A24). The raw reference text is deliberately not consulted; it differs by
// origin (relative for source, absolute for descriptors).
bool owns_map_entry(const SchemaFile& owner, const DescriptorPath& field_path,
                    const FieldDescriptorProto& field,
                    const SymbolEntry& entry,
                    const DescriptorProto& entry_message)
{
    if (!entry_message.options.has_map_entry ||
        !entry_message.options.map_entry) {
        return false;
    }
    if (entry.owner != &owner) return false;
    if (field.label != FieldDescriptorProto::LABEL_REPEATED) return false;
    ResolveContext context;
    context.owner = &owner;
    context.field_path = &field_path;
    context.field = &field;
    if (!nested_in_field_message(context, entry.path)) return false;
    return map_entry_name(view_text(field.name)) == entry_message.name;
}

// Type-dependent semantic validation for the textual default_value stored
// during parsing. Unknown kinds are the caller's responsibility: the caller
// passes unknown == true when the named type could not be resolved, in which
// case validation is deferred with a warning instead of incorrectly
// rejecting an imported packed enum or an imported enum default.
bool validate_default(SchemaFile& file, const SymbolIndex& symbols,
                      const std::set<const SchemaFile*>& visible_files,
                      const FieldDescriptorProto& field, bool unknown,
                      bool external, const DescriptorPath& field_path,
                      Diagnostic& error)
{
    if (!field.has_default_value) return true;
    // W-64: the position is computed on the first refusal and at most
    // once. The error-free path must not pay for it (the deliberate
    // laziness of 3b2e59e), and the exclusive branches below would
    // otherwise spell the same call nineteen times.
    SourceLocation where;
    bool have_where = false;
    const auto default_location = [&]() -> const SourceLocation& {
        if (!have_where) {
            where = field_default_location(file, field_path);
            have_where = true;
        }
        return where;
    };
    // The declared kind is known even when the definition is not, so the
    // kind-decided half of the rule is checked BEFORE the unknown-kind
    // deferral: compatibility mode leaves both flags true for an external
    // reference, and only the membership half needs the definition (W-47).
    // A message default is never legal, and an enum default must at least be
    // an identifier. The A5 warning already deferred membership to the
    // dependency header. A scalar kind cannot be external - the compatibility
    // classifier tolerates only message and enum references - so an unexpected
    // scalar falls through to the ordinary path below.
    const std::string value = view_text(field.default_value);
    if (external) {
        if (field.type == FieldDescriptorProto::TYPE_MESSAGE ||
            field.type == FieldDescriptorProto::TYPE_GROUP) {
            return fail_at(file, "message fields cannot have defaults",
                           default_location(),
                           field_path, error);
        }
        if (field.type == FieldDescriptorProto::TYPE_ENUM) {
            if (value.empty()) {
                return fail_at(file, "enum default must name an enum value",
                               default_location(),
                               field_path, error);
            }
            if (!is_identifier(value)) {
                return fail_at(file,
                               "Default value for an enum field must be an identifier.",
                               default_location(),
                               field_path, error);
            }
            return true;
        }
    }
    // An unresolvable named type may be an imported enum (whose default is
    // then legal) or an imported message (whose default complete linking
    // will reject). Either way validation is deferred with a warning instead
    // of failing on the unknown kind. Deferred parsing leaves type == 0, so
    // this check must precede the switch below.
    if (unknown) {
        warn_once(file,
                  kDeferredDefaultPrefix + view_text(field.type_name),
                  DIAGNOSTIC_GENERIC,
                  default_location(), field_path);
        return true;
    }
    if (field.type == FieldDescriptorProto::TYPE_STRING) {
        return true;
    }
    // W-40 (option B): every leading `+` on a numeric default is refused on
    // both paths. The source half is asked in the parser before the type
    // switch; this descriptor half is the one rule for a hand-written `.pbs`.
    // `protoc` itself accepts `+7` here because its integer path uses
    // `strtol`, but a descriptor generated from source never carries the
    // sign, so the stricter uniform answer is the owner's recorded decision.
    // The check is before the numeric switch, so `+1e400`/`+1e-400` are
    // refused before W-04's canonicalizer can see them (the adapter leaves
    // every leading `+` alone for the same reason).
    const bool numeric_default_type =
        field.type == FieldDescriptorProto::TYPE_FLOAT ||
        field.type == FieldDescriptorProto::TYPE_DOUBLE ||
        field.type == FieldDescriptorProto::TYPE_INT32 ||
        field.type == FieldDescriptorProto::TYPE_INT64 ||
        field.type == FieldDescriptorProto::TYPE_UINT32 ||
        field.type == FieldDescriptorProto::TYPE_UINT64 ||
        field.type == FieldDescriptorProto::TYPE_SINT32 ||
        field.type == FieldDescriptorProto::TYPE_SINT64 ||
        field.type == FieldDescriptorProto::TYPE_FIXED32 ||
        field.type == FieldDescriptorProto::TYPE_FIXED64 ||
        field.type == FieldDescriptorProto::TYPE_SFIXED32 ||
        field.type == FieldDescriptorProto::TYPE_SFIXED64;
    if (numeric_default_type && !value.empty() && value[0] == '+') {
        return fail_at(file, "numeric default value may not start with '+'",
                       default_location(), field_path,
                       error);
    }
    std::int32_t signed32 = 0;
    std::int64_t signed64 = 0;
    switch (field.type) {
        // D-58 (fix round r25): a bytes default is a C-escaped string, and
        // protoc validates its escape grammar while it builds the field. An
        // unchecked value reaches the generator verbatim, which is how
        // `[default = "\q"]` emitted `::std::string b = "\q";` into the
        // generated C++. The source path already refuses the twin as text
        // (`proto_parser.cpp:433`), so this is the descriptor-origin half of
        // that frontend parity rule.
        case FieldDescriptorProto::TYPE_BYTES:
            if (!valid_bytes_default_escape(value)) {
                return fail_at(file, "invalid escaping in bytes default value",
                               default_location(),
                               field_path, error);
            }
            return true;
        case FieldDescriptorProto::TYPE_BOOL:
            if (value != "true" && value != "false") {
                return fail_at(file, "bool default must be true or false",
                               default_location(),
                               field_path, error);
            }
            return true;
        case FieldDescriptorProto::TYPE_FLOAT:
        case FieldDescriptorProto::TYPE_DOUBLE:
            // W-03's last sub-case: a floating-point default whose stored text
            // starts with whitespace is refused, as the reference compiler
            // refuses it (`Couldn't parse default value " inf"`). `strtod`
            // skips the space, so `is_float_text` used to say yes and the
            // emitter's trim hid the shape. The source path cannot produce
            // it: the lexer skips whitespace before the token and stores the
            // value alone.
            if (!value.empty() && is_ascii_whitespace(value[0])) {
                return fail_at(file,
                               "floating-point default value may not have leading whitespace",
                               default_location(),
                               field_path, error);
            }
            // W-04: the validator accepts exactly the canonical form the
            // adapter and the source parser store. The old pair of predicates
            // (`is_float_text` plus a subnormal exception) asked a different
            // question - "would strtod parse this?" - and so accepted the
            // spellings CR-02 is about; the canonicalizer now owns the answer,
            // including the finite-subnormal case that reports ERANGE on
            // glibc.
            if (!is_canonical_float_default(
                    value, field.type == FieldDescriptorProto::TYPE_FLOAT)) {
                return fail_at(file, "invalid floating-point default",
                               default_location(),
                               field_path, error);
            }
            return true;
        case FieldDescriptorProto::TYPE_ENUM:
            if (value.empty()) {
                return fail_at(file, "enum default must name an enum value",
                               default_location(),
                               field_path, error);
            }
            // The identifier rule comes FIRST, exactly as the reference
            // compiler asks it (descriptor.cc's enum default branch): a
            // default of "1A" is a legal symbol name, so the membership check
            // alone would let it through and the emitter would write
            // `::p::E::1A` - not a C++ name - into the header.
            if (!is_identifier(value)) {
                return fail_at(file,
                               "Default value for an enum field must be an identifier.",
                               default_location(),
                               field_path, error);
            }
            {
                const std::string enum_name = view_text(field.type_name);
                const std::map<std::string, SymbolEntry>::const_iterator found =
                    symbols.symbols.find(enum_name);
                const EnumDescriptorProto* enumeration = 0;
                if (found != symbols.symbols.end() &&
                    found->second.kind == FieldDescriptorProto::TYPE_ENUM &&
                    visible_files.find(found->second.owner) != visible_files.end()) {
                    enumeration = find_enum(*found->second.owner, found->second.path);
                }
                if (enumeration == 0) {
                    return fail_at(file,
                                   "cannot validate default for unknown enum type " + enum_name,
                                   default_location(),
                                   field_path, error);
                }
                for (std::size_t i = 0; i < enumeration->value.size(); ++i) {
                    if (enumeration->value[i].name == field.default_value) return true;
                }
                return fail_at(file,
                               "enum type " + enum_name + " has no value named " + value,
                               default_location(),
                               field_path, error);
            }
        case FieldDescriptorProto::TYPE_MESSAGE:
        case FieldDescriptorProto::TYPE_GROUP:
            return fail_at(file, "message fields cannot have defaults",
                           default_location(),
                           field_path, error);
        case FieldDescriptorProto::TYPE_INT32:
        case FieldDescriptorProto::TYPE_SINT32:
        case FieldDescriptorProto::TYPE_SFIXED32:
            if (!parse_signed_32(value, signed32)) {
                return fail_at(file, "default does not fit signed 32-bit field",
                               default_location(),
                               field_path, error);
            }
            return true;
        case FieldDescriptorProto::TYPE_UINT32:
        case FieldDescriptorProto::TYPE_FIXED32:
            if (!parse_unsigned_with_optional_plus(value, 4294967295ull)) {
                return fail_at(file, "default does not fit unsigned 32-bit field",
                               default_location(),
                               field_path, error);
            }
            return true;
        case FieldDescriptorProto::TYPE_INT64:
        case FieldDescriptorProto::TYPE_SINT64:
        case FieldDescriptorProto::TYPE_SFIXED64:
            if (!parse_signed_64(value, signed64)) {
                return fail_at(file, "default does not fit signed 64-bit field",
                               default_location(),
                               field_path, error);
            }
            return true;
        case FieldDescriptorProto::TYPE_UINT64:
        case FieldDescriptorProto::TYPE_FIXED64:
            if (!parse_unsigned_with_optional_plus(value, 18446744073709551615ull)) {
                return fail_at(file, "default does not fit unsigned 64-bit field",
                               default_location(),
                               field_path, error);
            }
            return true;
        default:
            return fail_at(file, "unsupported field type in default validation",
                           default_location(),
                           field_path, error);
    }
}

// The orchestrator is declared first: validate_nested_types recurses into it.
bool resolve_message(SchemaFile& file, const SymbolIndex& symbols,
                     const std::set<const SchemaFile*>& visible_files,
                     const UnresolvedPolicy& unresolved_policy,
                     bool link_types,
                     DescriptorProto& message,
                     const std::string& parent, const DescriptorPath& message_path,
                     Diagnostic& error);

// 1. the shape a field declares - always asked, in both modes
bool validate_field_declared_shape(SchemaFile& file, FieldDescriptorProto& field,
                                   const DescriptorPath& field_path,
                                   bool is_proto3, Diagnostic& error)
{
    // D-23 (contract clause 12): field structure is validated over every
    // available field, in both modes, before any resolution, because the
    // shapes are provable from the descriptor alone and a
    // reference-driven check would never reach a field nothing names -
    // A26's entry-shape precedent. The number rule mirrors the parser's
    // source-path refusals; the named-kind rule has no source twin, since
    // a source field always carries the type its declaration gave it.
    // Both are asked in EVERY mode, which is what the compatibility mode
    // needs - and it is why fix round r12's compatibility-only block for
    // the named-kind shape was provably DEAD (fix round r17, the 0927-mimo
    // review's F1): a field whose declared kind is named and which carries
    // no type_name can never be classified as an external reference,
    // because the mode's single "maybe external" door is an ABSOLUTE type
    // name and this shape has no name at all. The rationale r12 recorded
    // lives here now, beside the check that actually answers it.
    if (field.number <= 0 || field.number > kMaxFieldNumber) {
        return fail_at_code(
            file,
            "field \"" + view_text(field.name) + "\" number " +
                number_text(field.number) +
                " is outside the valid range 1..536870911",
            declaration_location(file, field_path), field_path,
            DIAGNOSTIC_INVALID_FIELD_NUMBER, error);
    }
    if (field.number >= kReservedFieldNumberFirst &&
        field.number <= kReservedFieldNumberLast) {
        return fail_at_code(
            file,
            "field \"" + view_text(field.name) + "\" number " +
                number_text(field.number) +
                " is inside the reserved range 19000..19999",
            declaration_location(file, field_path), field_path,
            DIAGNOSTIC_INVALID_FIELD_NUMBER, error);
    }
    if (field.has_type && is_named_kind(field.type) &&
        !field.has_type_name) {
        return fail_at_code(
            file,
            "field \"" + view_text(field.name) + "\" declares " +
                descriptor_kind_name(field.type) +
                " type but carries no type_name",
            declaration_location(file, field_path), field_path,
            DIAGNOSTIC_MISSING_TYPE_NAME, error);
    }
    // D-55 (fix round r25): a repeated field may not carry a default, which
    // protoc refuses while it builds a descriptor ("Repeated fields can't
    // have default values.", descriptor.cc:6652-6655) and our parser
    // refuses as source text ("default value is not allowed on repeated
    // fields", proto_parser.cpp). Reference-free and asked in
    // BOTH modes: no resolution can turn a repeated field into a singular
    // one. It is asked BEFORE the proto3 block below, because protoc
    // reports this shape first - the repeated-default check lives in the
    // field build, the proto3 field rules in the later options validation.
    // Only a label the descriptor actually declared can be repeated here,
    // because protoc's own default for an absent label is OPTIONAL.
    if (field.has_default_value && field.has_label &&
        field.label == FieldDescriptorProto::LABEL_REPEATED) {
        return fail_at(file,
                       "repeated fields cannot have default values",
                       field_default_location(file, field_path), field_path,
                       error);
    }
    // Clause 12 (fix round r17, the 0927-mimo review's F2): a
    // descriptor-origin file that declares `syntax = "proto3"` must obey
    // the proto3 FIELD rules, asked at the same site and for the same
    // reason as the checks above - the shape is provable from the
    // descriptor alone, so both modes and every available field, before
    // anything is resolved. protoc asks both questions of every field of a
    // proto3 message when it builds the file from the same bytes
    // (`ValidateProto3Field`, descriptor.cc:7912-7930, reached from
    // `ValidateProto3Message`), and our parser refuses both as source text
    // ("required fields are not allowed in proto3",
    // codegen/parser/proto_parser.cpp:1329, and "explicit default values
    // are not allowed in proto3", :1192). Since S7e both source refusals
    // carry the same codes as the checks below (W-60), so the two origins
    // agree on the code as well as on the verdict. The gate is the field's
    // OWN file: proto2 gives both shapes a meaning, so reading the wrong
    // file here would refuse legal input (the proto2 controls in
    // test_descriptor_origin_proto3_field_rules_are_refused).
    //
    // The open-enum first-value rule is the same family and NOT here: the
    // contract sweep's row B6 assigns open-enum semantics to plan 2/5, and
    // ledger `D-33` records that scope decision (rejected), re-raised by
    // the same review rather than reversed by this round.
    if (is_proto3) {
        if (field.label == FieldDescriptorProto::LABEL_REQUIRED) {
            return fail_at_code(
                file,
                "field \"" + view_text(field.name) +
                    "\" is required, and a proto3 field may not be",
                declaration_location(file, field_path), field_path,
                DIAGNOSTIC_PROTO3_REQUIRED_FIELD, error);
        }
        if (field.has_default_value) {
            return fail_at_code(
                file,
                "field \"" + view_text(field.name) +
                    "\" carries an explicit default value, which proto3 "
                    "does not allow",
                field_default_location(file, field_path), field_path,
                DIAGNOSTIC_PROTO3_EXPLICIT_DEFAULT, error);
        }
        // D-54 (fix round r25): proto3 has no groups, which protoc refuses
        // from the same bytes ("Groups are not supported in proto3
        // syntax.", ValidateProto3Field, descriptor.cc:7936-7938). Our
        // source parser refuses the keyword in ANY syntax
        // (proto_parser.cpp:1240), so the shape arrives only as crafted
        // descriptor input, and the gate is the field's OWN file exactly as
        // its two siblings above. No dedicated code exists among the frozen
        // plan-4 values and adding one is a contract amendment, so the
        // failure carries DIAGNOSTIC_GENERIC like the other
        // kind-independent verdicts.
        if (field.type == FieldDescriptorProto::TYPE_GROUP) {
            return fail_at(file,
                           "field \"" + view_text(field.name) +
                               "\" is a group, and proto3 has no groups",
                           declaration_location(file, field_path), field_path,
                           error);
        }
    }
    // Plan 5 follow-up S1 (W-10): the kinds the EMITTER cannot spell. The
    // reference compiler resolves all three - an absent kind as the proto
    // default TYPE_DOUBLE, an unknown kind the same way, a group as a
    // generated group class - and emits working code; this project refuses
    // them, because the generator has no group types at all (the source
    // parser refuses a group outright: "group fields are not supported") and
    // because the emitter used to write "???type" / "???group" into the
    // header at rc=0. A deliberate "stricter than protoc" divergence, like
    // the package-depth and the output-root rules.
    //
    // The gate is the ORIGIN, not the shape: a source-origin field carries no
    // declared kind at all (deferred parsing leaves has_type unset and the
    // kind is written during resolution), so the same "no kind" test would
    // refuse every source field. The descriptor path is the only one that can
    // carry a kind the generator cannot use.
    //
    // The check sits AFTER the proto3 block above, so a proto3 group keeps
    // the proto3 wording and only a proto2 group reaches the rule below.
    if (file.from_descriptor_set) {
        if (!field.has_type) {
            return fail_at_code(
                file,
                "field \"" + view_text(field.name) +
                    "\" declares no type, so there is nothing to generate for it",
                declaration_location(file, field_path), field_path,
                DIAGNOSTIC_INVALID_FIELD_TYPE, error);
        }
        if (field.type == FieldDescriptorProto::TYPE_GROUP) {
            return fail_at_code(
                file,
                "field \"" + view_text(field.name) +
                    "\" is a group, and this generator does not support group types",
                declaration_location(file, field_path), field_path,
                DIAGNOSTIC_INVALID_FIELD_TYPE, error);
        }
        if (!is_known_field_kind(field.type)) {
            std::ostringstream text;
            text << "field \"" << view_text(field.name)
                 << "\" declares an unknown type (kind " << field.type << ")";
            return fail_at_code(
                file, text.str(), declaration_location(file, field_path),
                field_path, DIAGNOSTIC_INVALID_FIELD_TYPE, error);
        }
    }
    return true;
}

// 2. type resolution - only when link_types; reports the two outcome flags
bool resolve_field_type(SchemaFile& file, const SymbolIndex& symbols,
                        const std::set<const SchemaFile*>& visible_files,
                        const UnresolvedPolicy& unresolved_policy,
                        const std::string& scope, bool link_types,
                        FieldDescriptorProto& field, const DescriptorPath& field_path,
                        bool* unknown, bool* external_reference,
                        Diagnostic& error)
{
    // Set when the compatibility policy permitted an external reference:
    // the declared metadata is kept verbatim. The default is still checked
    // for what the declared kind decides (W-47); only membership of a named
    // enum value needs the definition, which the set does not carry.
    if (field.has_type_name) {
        if (!link_types) {
            // Deferred: never resolve locally. The resolved kind is
            // unknown until import linking, even when a local
            // declaration could match today.
            *unknown = true;
        } else {
            const std::string raw = raw_type_name(file, field, field_path);
            ResolveContext context;
            context.owner = &file;
            context.field_path = &field_path;
            context.field = &field;
            ResolveOutcome outcome = RESOLVE_MISSING;
            std::string resolved_name;
            const SymbolEntry* resolved = resolve_name(
                raw, scope, symbols, visible_files, context, &outcome,
                &resolved_name);
            if (resolved != 0) {
                // A7: a descriptor-origin field's declared kind is
                // authoritative metadata, so it is COMPARED with the kind
                // of the definition its name resolved to, never silently
                // rewritten. The comparison belongs before the assignment
                // below, which destroys the declared kind - after
                // resolution no post-pass can recover it. Source-origin
                // fields carry no declared kind (deferred parsing leaves
                // has_type unset), so they are overwritten as before.
                //
                // A group field declares TYPE_GROUP and names the group
                // message, and a group IS a message: protoc's own model
                // carries TYPE_GROUP only on the field. The index records
                // every message as TYPE_MESSAGE, so comparing the two
                // kinds literally used to refuse every legal proto2 group -
                // a divergence from the oracle, which accepts such a
                // descriptor (fix round r4, ledger D-12). The two kinds
                // therefore agree here, and the declared one is kept.
                //
                // The plan-5 follow-up's S1 (W-10) ended that exemption: a
                // descriptor-origin group is refused by the declared-shape
                // sweep before this point, because the generator has no
                // group types (the owner's decision of 2026-10-21, a
                // deliberate "stricter than protoc" divergence). The
                // comparison below therefore never sees a group, and the
                // exemption that made it agree with a message is gone with
                // it.
                if (file.from_descriptor_set && field.has_type &&
                    field.type != resolved->kind) {
                    return fail_at_code(
                        file,
                        kind_mismatch_message(field, resolved_name,
                                              resolved->kind),
                        field_type_location(file, field_path), field_path,
                        DIAGNOSTIC_KIND_MISMATCH, error);
                }
                // Step 6.1 (R11): the cross-syntax enum restriction. A
                // proto3 field must not directly use a proto2 enum, and
                // the rule keys on the file that DECLARES the enum, never
                // on the requesting file alone or on a transitive notion
                // of openness: a proto3 field may name a proto2 message
                // even when that message carries proto2 enum fields of its
                // own (oracle C3 accepts that shape), while a proto2 field
                // may use an enum of either syntax (S1, S2). The check
                // sits after the A7 comparison because the declared-kind
                // contradiction is the more specific verdict when a
                // descriptor carries both, and before the rewrite below
                // so a refused reference keeps its metadata.
                //
                // It is silent wherever the kind is unknown: the deferred
                // pass (link_types == false) resolves nothing, so `unknown`
                // still covers every named type, and the parser's own
                // allow_unresolved pass resolves only inside its own file,
                // whose syntax is the field's file's syntax by definition.
                // FILE_SYNTAX_UNSUPPORTED never matches on either side, so
                // a direct caller keeps A25's own verdict for an editions
                // file: link_schema refuses such a file before resolution,
                // and nothing here reads "not proto3" as proto2.
                //
                // No dedicated code exists for this verdict among the
                // seven plan-4 values (A3/A25/A26), each of which has a
                // frozen meaning that this rule would dilute - an eighth
                // value is a contract amendment, not a step - so the
                // failure carries DIAGNOSTIC_GENERIC like the other
                // kind-independent validation verdicts (packed legality,
                // default validation, duplicate field names).
                if (resolved->kind == FieldDescriptorProto::TYPE_ENUM &&
                    classify_file_syntax(file.file) == FILE_SYNTAX_PROTO3 &&
                    classify_file_syntax(resolved->owner->file) ==
                        FILE_SYNTAX_PROTO2) {
                    return fail_at(file,
                                   cross_syntax_enum_message(
                                       file, *resolved->owner,
                                       resolved_name),
                                   field_type_location(file, field_path),
                                   field_path, error);
                }
                // Authoritative metadata is not rewritten: the declared kind
                // stays the kind the descriptor named. A group never reaches
                // this line - the declared-shape sweep refuses it - so the
                // kind here is always the resolved definition's.
                field.type = resolved->kind;
                field.has_type = true;
                if (field.type_name != resolved_name) {
                    field.type_name = file.strings.save(resolved_name);
                }
            } else {
                *unknown = true;
                // A reference that exists but cannot be a field type is an
                // error even where a missing type is only deferred
                // (allow_unresolved): no import can turn a field, an enum
                // value, a service, or a foreign map entry into a legal
                // field type, and protoc rejects each of them today.
                // "Invisible" stays deferrable, because a later linking
                // pass is exactly what makes such a file visible.
                if (outcome == RESOLVE_NOT_A_TYPE ||
                    outcome == RESOLVE_MAP_ENTRY) {
                    return fail_at_code(
                        file,
                        reference_failure_message(file, symbols, raw, outcome,
                                                  resolved_name),
                        field_type_location(file, field_path), field_path,
                        outcome_code(outcome), error);
                }
                // A name the index HOLDS but the requesting file cannot
                // see is not "missing external information" either: the
                // descriptor says the type is declared, and protoc's own
                // verdict for the shape is an ERROR - the undeclared-
                // dependency branch of AddNotDefinedError (descriptor.cc
                // 4898-4929, libprotoc 30.1), the same "seems to be
                // defined in ..., which is not imported by ..., add the
                // necessary import" wording the INVISIBLE branch of
                // reference_failure_message mirrors. Tolerating it in
                // compatibility mode would record a warning whose
                // sentence is false (the type IS declared in the set) and
                // give one reference two verdicts depending on which
                // entry point is called (the review's CR-3, ledger
                // D-25), so the tolerance below is reserved for a
                // reference the index does not know at all
                // (RESOLVE_MISSING).
                const bool invisible_reference =
                    outcome == RESOLVE_INVISIBLE;
                const std::string reason =
                    invisible_reference
                        ? reference_failure_message(file, symbols, raw, outcome,
                                                    resolved_name)
                        : missing_reference_message(raw, scope, resolved_name);
                if (unresolved_policy.mode() == UnresolvedPolicy::EXTERNAL &&
                    !invisible_reference) {
                    // Step 7.2: compatibility mode tolerates a MISSING
                    // target only when the descriptor classifies the
                    // reference itself - an explicit supported message or
                    // enum kind and an absolute type name. Everything else
                    // (a relative source-style name, a missing has_type,
                    // and since fix round r12 an invisible declared type)
                    // is refused even here, or the mode becomes the
                    // general unresolved-type escape hatch the contract
                    // forbids. The code stays the lookup outcome's own
                    // (DIAGNOSTIC_UNRESOLVED_TYPE for a missing target,
                    // DIAGNOSTIC_INVISIBLE_TYPE for an invisible one):
                    // the reference is unresolved, the policy merely
                    // refuses to tolerate it.
                    const bool classified =
                        field.has_type && field.has_type_name &&
                        (field.type == FieldDescriptorProto::TYPE_MESSAGE ||
                         field.type == FieldDescriptorProto::TYPE_ENUM) &&
                        !field.type_name.empty() &&
                        field.type_name[0] == '.';
                    if (!classified) {
                        return fail_at_code(
                            file,
                            reason +
                            "; compatibility mode accepts an unresolved "
                            "type only when the descriptor supplies an "
                            "absolute type name and an explicit message or "
                            "enum kind",
                            field_type_location(file, field_path),
                            field_path, outcome_code(outcome), error);
                    }
                    // A5: the bound is one warning per type NAME, not per
                    // field, and it is the caller-owned set that carries
                    // it - warn_once only keeps a repeated pass idempotent.
                    // The warning goes to the stable unknown location
                    // because a thin descriptor has no positions, and the
                    // reference is never rewritten: no fabricated kind, no
                    // absolute name invented, no first enumerator or
                    // integer zero for its default.
                    *external_reference = true;
                    if (unresolved_policy.missing_external() != 0) {
                        *unresolved_policy.missing_external() = true;
                    }
                    const std::string type_name = view_text(field.type_name);
                    if (unresolved_policy.warned_types()
                            ->insert(type_name).second) {
                        warn_once(file, external_reference_message(type_name),
                                  DIAGNOSTIC_GENERIC, unknown_location(),
                                  field_path);
                    }
                } else if (unresolved_policy.mode() ==
                           UnresolvedPolicy::DEFERRED) {
                    warn_once(file,
                              reason +
                              "; it is kept as TYPE_MESSAGE until imports are linked",
                              outcome_code(outcome),
                              field_type_location(file, field_path), field_path);
                } else {
                    return fail_at_code(file, reason,
                                        field_type_location(file, field_path),
                                        field_path, outcome_code(outcome),
                                        error);
                }
            }
        }
    }
    return true;
}

// 3. the field rules that run after resolution - packed legality, the oneof
// index and label checks, and default validation
bool validate_field_encoding(SchemaFile& file, const SymbolIndex& symbols,
                             const std::set<const SchemaFile*>& visible_files,
                             const std::string& scope,
                             const DescriptorProto& message, bool link_types,
                             FieldDescriptorProto& field, const DescriptorPath& field_path,
                             bool unknown, bool external_reference,
                             Diagnostic& error)
{
    // Repeatedness is known without resolution: no import can turn an
    // optional field into a repeated one, so reject packed on
    // non-repeated fields immediately. Whether the type itself admits
    // packed encoding is checked only for known kinds.
    //
    // D-51 (fix round r25): the gate is protoc's `field->options().packed()`
    // - presence AND value - not `has_packed` alone. protoc refuses exactly
    // one shape, `[packed = true]` on a non-packable field
    // (descriptor.cc:7977), and writes `has_packed = true, packed = false`
    // for `[packed = false]`, so a presence-only gate refused the sets
    // protoc itself emits for an optional field, a repeated string and a
    // repeated message.
    if (field.options.has_packed && field.options.packed) {
        if (field.label != FieldDescriptorProto::LABEL_REPEATED) {
            return fail_at(file,
                           "packed option is valid only on repeated primitive or enum fields",
                           field_packed_location(file, field_path),
                           field_path, error);
        }
        if (!unknown && !packable_type(field.type)) {
            return fail_at(file,
                           "packed option is valid only on repeated primitive or enum fields",
                           field_packed_location(file, field_path),
                           field_path, error);
        }
    }
    if (field.has_oneof_index &&
        (field.oneof_index < 0 ||
         static_cast<std::size_t>(field.oneof_index) >= message.oneof_decl.size())) {
        return fail_at(file, "invalid oneof_index in message " + scope,
                       declaration_location(file, field_path), field_path, error);
    }
    // D-56 (fix round r25): a field claimed by a oneof must itself be
    // OPTIONAL, which protoc reports as "Fields of oneofs must themselves
    // have label LABEL_OPTIONAL." (descriptor.cc:7471-7475) - a shape only a
    // manually built descriptor can carry, because the .proto parser cannot
    // write it. The range check above keeps its own message by running
    // first; an ABSENT label is read as OPTIONAL, exactly as protoc's
    // `proto.label()` default is, so only a declared label can disagree.
    if (field.has_oneof_index && field.has_label &&
        field.label != FieldDescriptorProto::LABEL_OPTIONAL) {
        return fail_at(file,
                       "a field of a oneof must have label "
                       "LABEL_OPTIONAL",
                       declaration_location(file, field_path), field_path,
                       error);
    }
    // Scalar kinds are known without linking; named kinds are validated
    // only when linking (link_types) so a shadowed local match cannot
    // decide packed/default validity ahead of import linking. A tolerated
    // external reference still reaches the rule (W-47): the declared kind is
    // known, so the message/enum half of the default rule applies even
    // though the definition does not.
    if (!field.has_type_name) {
        if (!validate_default(file, symbols, visible_files, field, false, false,
                              field_path, error)) {
            return false;
        }
    } else if (link_types) {
        if (!validate_default(file, symbols, visible_files, field, unknown,
                              external_reference, field_path, error)) {
            return false;
        }
    }
    return true;
}

// 4. oneof declarations claimed by no field (D-28) - after the field loop on
// purpose
bool validate_message_oneofs(SchemaFile& file, const DescriptorProto& message,
                             const std::string& scope,
                             const DescriptorPath& message_path, Diagnostic& error)
{
    // D-28 (fix round r13, contract clause 12): every oneof declaration must be
    // claimed by at least one field. The index-RANGE check inside the loop
    // above only refuses a field pointing outside the list, so a declaration
    // nothing points at is invisible to it, while protoc refuses the same shape
    // when it builds a descriptor (`Oneof must have at least one field.`).
    //
    // It sits after the loop on purpose: a message that carries BOTH a dangling
    // index and an unclaimed declaration is still reported by the more specific
    // index rule first. It runs in both modes over every message, referenced or
    // not, and the deferred pass reaches it too - which is how the SOURCE half
    // is covered, since the parser accepts an empty oneof body as text while
    // protoc does not.
    for (std::size_t i = 0; i < message.oneof_decl.size(); ++i) {
        bool claimed = false;
        for (std::size_t j = 0; j < message.field.size() && !claimed; ++j) {
            claimed = message.field[j].has_oneof_index &&
                      message.field[j].oneof_index ==
                          static_cast<std::int32_t>(i);
        }
        if (!claimed) {
            const DescriptorPath oneof_path =
                message_oneof_path(message_path, static_cast<int>(i));
            return fail_at_code(
                file,
                "oneof \"" + view_text(message.oneof_decl[i].name) +
                    "\" in " + scope + " has no fields",
                declaration_location(file, oneof_path), oneof_path,
                DIAGNOSTIC_EMPTY_ONEOF, error);
        }
    }
    return true;
}

// 5. nested type names, then recursion
bool validate_nested_types(SchemaFile& file, const SymbolIndex& symbols,
                           const std::set<const SchemaFile*>& visible_files,
                           const UnresolvedPolicy& unresolved_policy, bool link_types,
                           DescriptorProto& message, const std::string& scope,
                           const DescriptorPath& message_path, Diagnostic& error)
{
    std::set<str_view> nested_names;
    for (std::size_t i = 0; i < message.enum_type.size(); ++i) {
        if (!nested_names.insert(message.enum_type[i].name).second) {
            const DescriptorPath enum_path =
                message_enum_path(message_path, static_cast<int>(i));
            return fail_at(file, "duplicate nested type name in " + scope,
                           declaration_location(file, enum_path),
                           enum_path, error);
        }
    }
    for (std::size_t i = 0; i < message.nested_type.size(); ++i) {
        const DescriptorPath nested_path =
            message_nested_path(message_path, static_cast<int>(i));
        if (!nested_names.insert(message.nested_type[i].name).second) {
            return fail_at(file, "duplicate nested type name in " + scope,
                           declaration_location(file, nested_path),
                           nested_path, error);
        }
        if (!resolve_message(file, symbols, visible_files, unresolved_policy,
                             link_types,
                             message.nested_type[i], scope, nested_path, error)) {
            return false;
        }
    }
    return true;
}

// One endpoint of one service method (`W-16`). protoc's `CrossLinkMethod`
// (descriptor.cc:7714-7757) resolves both endpoints in the method's own scope
// with a message placeholder: a name that resolves nowhere is "not defined",
// and a name that resolves to anything but a message is "not a message type".
// The source frontend used to discard the method table, so neither shape was
// reachable from `.proto`; the model retains the names now, and this is the
// rule both frontends answer.
//
// What a name that resolves nowhere means is the caller's policy, exactly as
// it is for a field: the parser's standalone pass warns and keeps the raw text
// (deferred), compatibility tolerates only an absolute name (a descriptor
// carries no kind for a method endpoint, so the absolute spelling is the one
// fact that says the file meant an external message), and complete linking
// refuses. A non-type declaration and an invisible declaration are fatal in
// every mode: no import can turn them into a message, and an invisible declared
// type is the undeclared-dependency shape protoc refuses. A synthetic map entry
// is NOT fatal here: the ownership rule is a field rule, so a method endpoint
// names an ordinary message type (CR-21).
bool resolve_method_type(SchemaFile& file, const SymbolIndex& symbols,
                         const std::set<const SchemaFile*>& visible_files,
                         const UnresolvedPolicy& unresolved_policy,
                         bool link_types, const std::string& method_scope,
                         const char* endpoint, const DescriptorPath& type_path,
                         str_view& declared, Diagnostic& error)
{
    if (!link_types) return true;

    const std::string raw = view_text(declared);
    const std::string subject =
        "method \"" + method_scope + "\" " + endpoint + " type";
    ResolveContext context;
    ResolveOutcome outcome = RESOLVE_MISSING;
    std::string resolved_name;
    const SymbolEntry* resolved =
        resolve_name(raw, method_scope, symbols, visible_files, context,
                     &outcome, &resolved_name);
    if (resolved != 0) {
        if (resolved->kind != FieldDescriptorProto::TYPE_MESSAGE) {
            // The code is the field rule's own: the declaration resolves, but
            // cannot stand where a message is required. protoc's sentence is
            // `"<name>" is not a message type.`
            return fail_at_code(
                file,
                subject + " \"" + raw + "\" resolves to \"" + resolved_name +
                    "\", which is not a message type",
                declaration_location(file, type_path), type_path,
                DIAGNOSTIC_NOT_A_TYPE, error);
        }
        // CR-48: an endpoint is stored the way the field type is - the
        // canonical absolute name the descriptor path carries. Without this,
        // `--print-descriptor` would spell a source method's endpoint as
        // written (`Request`) and a descriptor method's as `.pkg.Request`, so
        // the two frontends would disagree on the canonical text (P2).
        if (declared != resolved_name) {
            declared = file.strings.save(resolved_name);
        }
        return true;
    }
    if (outcome != RESOLVE_MISSING) {
        return fail_at_code(
            file,
            subject + " \"" + raw + "\": " +
                reference_failure_message(file, symbols, raw, outcome,
                                          resolved_name),
            declaration_location(file, type_path), type_path,
            outcome_code(outcome), error);
    }

    const std::string reason =
        subject + ": " + missing_reference_message(raw, method_scope,
                                                   resolved_name);
    if (unresolved_policy.mode() == UnresolvedPolicy::EXTERNAL) {
        // The compatibility half. Unlike a field, a method endpoint carries no
        // declared kind to classify, so the absolute spelling is the whole
        // test - a relative name is source-style metadata and is refused, as
        // a relative field type is.
        const bool classified = !declared.empty() && declared[0] == '.';
        if (!classified) {
            return fail_at_code(
                file,
                reason +
                    "; compatibility mode accepts an unresolved method type "
                    "only when the descriptor supplies an absolute type name",
                declaration_location(file, type_path), type_path,
                DIAGNOSTIC_UNRESOLVED_TYPE, error);
        }
        if (unresolved_policy.missing_external() != 0) {
            *unresolved_policy.missing_external() = true;
        }
        if (unresolved_policy.warned_types()->insert(raw).second) {
            warn_once(file, external_reference_message(raw), DIAGNOSTIC_GENERIC,
                      unknown_location(), type_path);
        }
        return true;
    }
    if (unresolved_policy.mode() == UnresolvedPolicy::DEFERRED) {
        warn_once(file,
                  reason + "; the endpoint is kept unresolved until imports "
                           "are linked",
                  DIAGNOSTIC_UNRESOLVED_TYPE,
                  declaration_location(file, type_path), type_path);
        return true;
    }
    return fail_at_code(file, reason, declaration_location(file, type_path),
                        type_path, DIAGNOSTIC_UNRESOLVED_TYPE, error);
}

// The service half of a resolution pass (`W-16`): every retained method's
// input and output endpoint, in declaration order. It runs after the message
// walk, so an endpoint may name any message of the file (the index is complete
// before either walk starts). A missing endpoint field is refused in every
// mode: the declaration does not say what the method takes or returns, and
// only a descriptor can carry that shape.
bool resolve_services(SchemaFile& file, const SymbolIndex& symbols,
                      const std::set<const SchemaFile*>& visible_files,
                      const UnresolvedPolicy& unresolved_policy,
                      bool link_types, Diagnostic& error)
{
    const std::string prefix = package_prefix(file);
    for (std::size_t i = 0; i < file.file.service.size(); ++i) {
        ServiceDescriptorProto& service = file.file.service[i];
        const DescriptorPath service_path = file_service_path(static_cast<int>(i));
        const std::string service_scope = joined_name(prefix, service.name);
        for (std::size_t j = 0; j < service.method.size(); ++j) {
            MethodDescriptorProto& method = service.method[j];
            const DescriptorPath method_path =
                service_method_path(service_path, static_cast<int>(j));
            const std::string method_scope =
                joined_name(service_scope, method.name);
            if (!method.has_input_type || method.input_type.empty()) {
                return fail_at_code(
                    file,
                    "method \"" + method_scope + "\" carries no input type "
                    "name",
                    declaration_location(file, method_path), method_path,
                    DIAGNOSTIC_MISSING_TYPE_NAME, error);
            }
            if (!method.has_output_type || method.output_type.empty()) {
                return fail_at_code(
                    file,
                    "method \"" + method_scope + "\" carries no output type "
                    "name",
                    declaration_location(file, method_path), method_path,
                    DIAGNOSTIC_MISSING_TYPE_NAME, error);
            }
            if (!resolve_method_type(
                    file, symbols, visible_files, unresolved_policy, link_types,
                    method_scope, "input",
                    service_method_type_path(method_path, 2), method.input_type,
                    error)) {
                return false;
            }
            if (!resolve_method_type(
                    file, symbols, visible_files, unresolved_policy, link_types,
                    method_scope, "output",
                    service_method_type_path(method_path, 3),
                    method.output_type, error)) {
                return false;
            }
        }
    }
    return true;
}

// Checks every field in one message subtree. With link_types == true this
// resolves custom field TypeNames and applies the semantic rules that
// require the resolved kind (packed legality for named types, named default
// validation). With link_types == false only checks that need no type
// resolution run: a locally resolvable kind must not decide the outcome,
// because shadowing imports may change it after linking. Unknown-kind
// packed/default rules are then left for complete linking.
//
// `unresolved_policy` says what a type_name that resolves nowhere means to
// the pass that called this (see UnresolvedPolicy above).
bool resolve_message(SchemaFile& file, const SymbolIndex& symbols,
                     const std::set<const SchemaFile*>& visible_files,
                     const UnresolvedPolicy& unresolved_policy,
                     bool link_types,
                     DescriptorProto& message,
                     const std::string& parent, const DescriptorPath& message_path,
                     Diagnostic& error)
{
    const std::string scope = joined_name(parent, message.name);
    const bool is_proto3 = classify_file_syntax(file.file) == FILE_SYNTAX_PROTO3;
    std::set<str_view> names;
    std::set<std::int32_t> numbers;
    for (std::size_t i = 0; i < message.field.size(); ++i) {
        FieldDescriptorProto& field = message.field[i];
        const DescriptorPath field_path =
            message_field_path(message_path, static_cast<int>(i));
        if (!names.insert(field.name).second) {
            return fail_at(file, "duplicate field name " + view_text(field.name),
                           declaration_location(file, field_path), field_path, error);
        }
        if (!numbers.insert(field.number).second) {
            return fail_at(file, "duplicate field number in message " + scope,
                           declaration_location(file, field_path), field_path, error);
        }

        bool unknown = false;
        bool external_reference = false;
        if (!validate_field_declared_shape(file, field, field_path, is_proto3,
                                           error)) {
            return false;
        }
        if (!resolve_field_type(file, symbols, visible_files, unresolved_policy,
                                scope, link_types, field, field_path, &unknown,
                                &external_reference, error)) {
            return false;
        }
        if (!validate_field_encoding(file, symbols, visible_files, scope, message,
                                     link_types, field, field_path, unknown,
                                     external_reference, error)) {
            return false;
        }
    }

    if (!validate_message_oneofs(file, message, scope, message_path, error)) {
        return false;
    }
    return validate_nested_types(file, symbols, visible_files, unresolved_policy,
                                 link_types, message, scope, message_path, error);
}

// One-line bridges onto the anonymous message-level validators declared in
// schema_semantics_internal.hpp. The call is qualified with the enclosing
// namespace because an unqualified name here would find the forwarder itself;
// `easypb_schema::name` resolves to the anonymous function through the unnamed
// namespace's implicit using-directive. The definitions stay anonymous.
bool validate_message_symbol_names(const SchemaFile& file,
                                   const DescriptorProto& message,
                                   const DescriptorPath& message_path,
                                   const std::string& parent, Diagnostic& error)
{
    return ::easypb_schema::validate_message_symbol_names(
        file, message, message_path, parent, error);
}

bool validate_message_enum_value_numbers(const SchemaFile& file,
                                         const DescriptorProto& message,
                                         const std::string& qualified_name,
                                         const DescriptorPath& message_path,
                                         Diagnostic& error)
{
    return ::easypb_schema::validate_message_enum_value_numbers(
        file, message, qualified_name, message_path, error);
}

bool validate_message_map_entries(const SchemaFile& file,
                                  const DescriptorProto& message,
                                  const std::string& qualified_name,
                                  Diagnostic& error)
{
    return ::easypb_schema::validate_message_map_entries(file, message,
                                                         qualified_name, error);
}

} // namespace internal

// The public passes below keep their unqualified spellings: the rule block
// moved into `internal`, and these using-declarations are the one-line bridge.
using internal::UnresolvedPolicy;
using internal::resolve_message;
using internal::resolve_services;

bool validate_map_entry_shapes(SchemaFile& file, Diagnostic& error)
{
    // Every top-level message is visited in declaration order, and the
    // recursion visits an entry before its own nested types, so the first
    // malformed entry is the first one in the file's declaration order. No
    // symbol index and no visibility set is consulted: entry shape is provable
    // from the available descriptor alone, which is why it runs before any
    // resolution and in both linking modes.
    const std::string prefix = package_prefix(file);
    for (std::size_t i = 0; i < file.file.message_type.size(); ++i) {
        if (!validate_message_map_entries(
                file, file.file.message_type[i],
                joined_name(prefix, file.file.message_type[i].name),
                error)) {
            return false;
        }
    }
    return true;
}

bool validate_enum_value_numbers(SchemaFile& file, Diagnostic& error)
{
    // The same reference-free shape of walk as validate_map_entry_shapes:
    // top-level enums in declaration order, then each top-level message and its
    // nested types recursively, so the first malformed enum is the first one in
    // the file's declaration order. No symbol index and no visibility set is
    // consulted - the shape is provable from the available descriptor alone,
    // which is why it runs before any resolution and in both linking modes.
    const std::string prefix = package_prefix(file);
    for (std::size_t i = 0; i < file.file.enum_type.size(); ++i) {
        if (!validate_enum_value_numbers_in_enum(
                file, file.file.enum_type[i],
                joined_name(prefix, file.file.enum_type[i].name),
                file_enum_path(static_cast<int>(i)), error)) {
            return false;
        }
    }
    for (std::size_t i = 0; i < file.file.message_type.size(); ++i) {
        if (!validate_message_enum_value_numbers(
                file, file.file.message_type[i],
                joined_name(prefix, file.file.message_type[i].name),
                file_message_path(static_cast<int>(i)), error)) {
            return false;
        }
    }
    return true;
}

namespace {

std::string json_conflict_message(const FieldDescriptorProto& later,
                                  const std::string& later_json,
                                  bool later_custom,
                                  const FieldDescriptorProto& earlier,
                                  const std::string& earlier_json,
                                  bool earlier_custom)
{
    const char* later_kind = later_custom ? "custom" : "default";
    const char* earlier_kind = earlier_custom ? "custom" : "default";
    std::string message =
        std::string("The ") + later_kind + " JSON name of field \"" +
        view_text(later.name) + "\" (\"" + later_json +
        "\") conflicts with the " + earlier_kind +
        " JSON name of field \"" + view_text(earlier.name) + "\"";
    if (later_json != earlier_json) {
        message += " (\"" + earlier_json + "\")";
    }
    message += ".";
    return message;
}

bool validate_message_json_names(SchemaFile& file,
                                 const DescriptorProto& message,
                                 const DescriptorPath& message_path,
                                 Diagnostic& error)
{
    const bool is_proto3 =
        classify_file_syntax(file.file) == FILE_SYNTAX_PROTO3;
    const std::size_t count = message.field.size();
    std::vector<std::string> default_names;
    default_names.reserve(count);

    // Pass 1: every field's default JSON name. A collision is an error in
    // proto3 and the reference compiler's LEGACY_BEST_EFFORT warning in
    // proto2; generation continues in the latter case.
    std::map<std::string, std::size_t> default_by_name;
    for (std::size_t i = 0; i < count; ++i) {
        const FieldDescriptorProto& field = message.field[i];
        default_names.push_back(to_json_name(view_text(field.name)));
        const std::pair<std::map<std::string, std::size_t>::iterator, bool>
            inserted = default_by_name.insert(
                std::map<std::string, std::size_t>::value_type(
                    default_names.back(), i));
        if (inserted.second) continue;

        const FieldDescriptorProto& earlier =
            message.field[inserted.first->second];
        const DescriptorPath field_path =
            message_field_path(message_path, static_cast<int>(i));
        const std::string message_text = json_conflict_message(
            field, default_names.back(), false, earlier,
            default_names[inserted.first->second], false);
        if (is_proto3) {
            return fail_at_code(
                file, message_text, declaration_location(file, field_path),
                field_path, DIAGNOSTIC_DUPLICATE_SYMBOL, error);
        }
        warn_once(file, message_text, DIAGNOSTIC_DUPLICATE_SYMBOL,
                  declaration_location(file, field_path), field_path);
    }

    // Pass 2: the effective names. A pair of two default names was already
    // reported by pass 1 and is skipped; every pair involving a custom name
    // follows the same proto2-warning/proto3-error split, except that two
    // custom names are an error in both syntaxes.
    std::map<std::string, std::size_t> effective_by_name;
    for (std::size_t i = 0; i < count; ++i) {
        const FieldDescriptorProto& field = message.field[i];
        const std::string default_name = default_names[i];
        const bool custom =
            field.has_json_name &&
            view_text(field.json_name) != default_name;
        const std::string effective =
            custom ? view_text(field.json_name) : default_name;
        // protoc's `JsonNameLooksLikeExtension`: the bracket spelling is the
        // extension form, so a CUSTOM name cannot use it (CR-22). Only a custom
        // name can reach this rule: a default name comes from a field name and
        // therefore starts with an identifier character.
        if (custom && effective.size() >= 2 && effective[0] == '[' &&
            effective[effective.size() - 1] == ']') {
            const DescriptorPath field_path =
                message_field_path(message_path, static_cast<int>(i));
            return fail_at_code(
                file,
                "The custom JSON name of field \"" + view_text(field.name) +
                    "\" (\"" + effective +
                    "\") is invalid: JSON names may not start with '[' and "
                    "end with ']'",
                declaration_location(file, field_path), field_path,
                DIAGNOSTIC_INVALID_JSON_NAME, error);
        }
        const std::pair<std::map<std::string, std::size_t>::iterator, bool>
            inserted = effective_by_name.insert(
                std::map<std::string, std::size_t>::value_type(effective, i));
        if (inserted.second) continue;

        const std::size_t earlier_index = inserted.first->second;
        const FieldDescriptorProto& earlier = message.field[earlier_index];
        const bool earlier_custom =
            earlier.has_json_name &&
            view_text(earlier.json_name) != default_names[earlier_index];
        if (!custom && !earlier_custom) continue;

        const DescriptorPath field_path =
            message_field_path(message_path, static_cast<int>(i));
        const std::string message_text = json_conflict_message(
            field, effective, custom, earlier,
            earlier_custom ? view_text(earlier.json_name)
                           : default_names[earlier_index],
            earlier_custom);
        if (is_proto3 || (custom && earlier_custom)) {
            return fail_at_code(
                file, message_text, declaration_location(file, field_path),
                field_path, DIAGNOSTIC_DUPLICATE_SYMBOL, error);
        }
        warn_once(file, message_text, DIAGNOSTIC_DUPLICATE_SYMBOL,
                  declaration_location(file, field_path), field_path);
    }

    for (std::size_t i = 0; i < message.nested_type.size(); ++i) {
        if (!validate_message_json_names(
                file, message.nested_type[i],
                message_nested_path(message_path, static_cast<int>(i)),
                error)) {
            return false;
        }
    }
    return true;
}

} // namespace

bool validate_json_names(SchemaFile& file, Diagnostic& error)
{
    // W-43: the reference compiler runs two passes per message
    // (`CheckFieldJsonNameUniqueness`, descriptor.cc:6489-6584): default
    // names first, then the effective names with custom `json_name` values.
    // This walk mirrors that shape over the available descriptor alone.
    for (std::size_t i = 0; i < file.file.message_type.size(); ++i) {
        if (!validate_message_json_names(
                file, file.file.message_type[i],
                file_message_path(static_cast<int>(i)), error)) {
            return false;
        }
    }
    return true;
}

bool validate_extension_ranges(SchemaFile& file, Diagnostic& error)
{
    // The same reference-free walk as validate_enum_value_numbers: top-level
    // messages in declaration order, then their nested types recursively. No
    // symbol index and no visibility set is consulted - a range and a declared
    // field number are provable from the available descriptor alone - so the
    // check runs before any resolution and in both linking modes (W-45).
    const std::string prefix = package_prefix(file);
    const bool is_proto3 =
        classify_file_syntax(file.file) == FILE_SYNTAX_PROTO3;
    for (std::size_t i = 0; i < file.file.message_type.size(); ++i) {
        if (!validate_message_extension_ranges(
                file, file.file.message_type[i],
                joined_name(prefix, file.file.message_type[i].name),
                file_message_path(static_cast<int>(i)), is_proto3, error)) {
            return false;
        }
    }
    return true;
}

bool validate_reserved_declarations(SchemaFile& file, Diagnostic& error)
{
    // Reference-free, like the extension-range sweep beside it: a reserved
    // range, a reserved name and a declared number are provable from the
    // available descriptor alone, whether or not anything references the
    // declaration (step B5, CR-06). The walk is top-level enums, then
    // top-level messages in declaration order with their nested types, the
    // shape validate_enum_value_numbers uses.
    const std::string prefix = package_prefix(file);
    for (std::size_t i = 0; i < file.file.enum_type.size(); ++i) {
        if (!validate_enum_reserved(
                file, file.file.enum_type[i],
                joined_name(prefix, file.file.enum_type[i].name),
                file_enum_path(static_cast<int>(i)), error)) {
            return false;
        }
    }
    for (std::size_t i = 0; i < file.file.message_type.size(); ++i) {
        if (!validate_message_reserved(
                file, file.file.message_type[i],
                joined_name(prefix, file.file.message_type[i].name),
                file_message_path(static_cast<int>(i)), error)) {
            return false;
        }
    }
    return true;
}

bool validate_message_set_wire_format(SchemaFile& file, Diagnostic& error)
{
    // Same reference-free shape: the option and the field count are both in
    // the descriptor, and a MessageSet that declares fields is malformed even
    // when nothing references the message (step B5, CR-23).
    const std::string prefix = package_prefix(file);
    for (std::size_t i = 0; i < file.file.message_type.size(); ++i) {
        if (!validate_message_set_in_message(
                file, file.file.message_type[i],
                joined_name(prefix, file.file.message_type[i].name),
                file_message_path(static_cast<int>(i)), error)) {
            return false;
        }
    }
    return true;
}

bool validate_message_nesting_depth(const FileDescriptorProto& file,
                                    const std::string& label,
                                    Diagnostic& error)
{
    // The reference compiler refuses the 32nd level of nested messages with
    // "Reached maximum recursion limit for nested messages."
    // (compiler/parser.cc:785-789 for the source frontend,
    // descriptor.cc:6364-6373 for the descriptor one), and its limit is
    // MaxMessageDeclarationNestingDepth() == 32 (descriptor.h:3045). The
    // sentence is the reference compiler's own, so a reader compares the two
    // verdicts without translating between them.
    DescriptorPath offending;
    const int depth = deepest_message_nesting(file, offending);
    if (depth < max_message_nesting_depth()) return true;
    error.file = label;
    error.location = unknown_location();
    error.message = "Reached maximum recursion limit for nested messages.";
    if (!offending.empty()) error.message += " at " + format_path(offending);
    error.warning = false;
    error.code = DIAGNOSTIC_GENERIC;
    return false;
}

bool validate_symbol_names(SchemaFile& file, Diagnostic& error)
{
    // protoc validates every declaration name lexically while it builds the
    // file (`ValidateSymbolName`, descriptor.cc:5419-5438): an empty name is
    // "Missing name." and any character outside `[A-Za-z0-9_]` is `"<name>" is
    // not a valid identifier.`. The reference calls it at every build site -
    // package components (`:5400`/`:5404`), messages (`:6325`), fields
    // (`:6604`), oneofs (`:6942`), enums (`:7032`), enum values (`:7147`),
    // services (`:7208`) - and imposes no first-character rule, so neither does
    // this. The check rides the same reference-free stage as A26's entry shape
    // and D-28's enum numbers: a malformed name is provable from the available
    // descriptor alone and an unreferenced declaration still reaches the
    // generator, so it runs before any resolution in BOTH modes (fix round r18,
    // ledger `D-39`, contract clause 13). The file's own `name` is a logical
    // path, not a symbol, and is NOT checked here.
    const std::string prefix = package_prefix(file);
    const str_view& package = file.file.package;
    if (file.file.has_package && !package.empty()) {
        // W-38: protoc's limit counts DOTS, not components
        // (`kPackageLimit = 100`, descriptor.cc:96; `std::count(...) >
        // kPackageLimit`, :6024-6030). The accepted boundary is therefore
        // 101 components and the first refusal is 102, measured against
        // libprotoc 30.1. The rule runs in this reference-free sweep so both
        // link modes and both input paths answer it before indexing.
        const std::size_t dots =
            static_cast<std::size_t>(std::count(package.begin(),
                                                package.end(), '.'));
        if (dots > 100u) {
            static const DescriptorPath package_path = {2};
            return fail_at(file,
                           "package depth exceeds the maximum of 101 components",
                           declaration_location(file, package_path),
                           package_path, error);
        }
        // D-59 (fix round r25): a package component belongs to the package, so
        // its scope is the package's own name, and its location is the
        // package field's descriptor path ([2], the path
        // `index_package_scopes` already uses) rather than an empty one. The
        // old call passed `std::string()` and `DescriptorPath()`, which read as
        // "in the global scope ... at []".
        // One read-only vector for every call, not one rebuilt per file: the
        // per-file form cost a 4-byte allocation for every file that declares a
        // package, whether or not this check ever has anything to report, and
        // `check_symbol_name` only reads the path.
        //
        // The vector is spelled as an initializer list, not as
        // `DescriptorPath(1, 2)`: the two-argument form also reads as the
        // iterator-pair constructor, and clang 3.1 cannot discard that
        // reading - libstdc++ 4.8's iterator_traits of a non-iterator is a
        // hard error there, not a substitution failure, so the lane died with
        // "no type named 'iterator_category' in 'std::iterator_traits<int>'"
        // (measured: the clang 3.1 lane of 2026-10-22). The list form is the
        // one reading that cannot be mistaken for the other: one element, the
        // value 2 - the package field's descriptor path.
        static const DescriptorPath package_path = {2};
        std::size_t start = 0;
        for (;;) {
            const std::size_t dot = package.find('.', start);
            const str_view component =
                package.substr(start, (dot == str_view::npos
                                           ? package.size()
                                           : dot) - start);
            if (!check_symbol_name(file, component, "package component",
                                   prefix, package_path, error)) {
                return false;
            }
            if (dot == str_view::npos) break;
            start = dot + 1;
        }
    }
    for (std::size_t i = 0; i < file.file.enum_type.size(); ++i) {
        if (!is_valid_symbol_name(file.file.enum_type[i].name)) {
            return fail_symbol_name(
                file, file.file.enum_type[i].name, "enum type", prefix,
                file_enum_path(static_cast<int>(i)), error);
        }
        for (std::size_t j = 0; j < file.file.enum_type[i].value.size(); ++j) {
            if (!is_valid_symbol_name(file.file.enum_type[i].value[j].name)) {
                return fail_symbol_name(
                    file, file.file.enum_type[i].value[j].name, "enum value",
                    prefix,
                    enum_value_path(file_enum_path(static_cast<int>(i)),
                                    static_cast<int>(j)),
                    error);
            }
        }
    }
    for (std::size_t i = 0; i < file.file.message_type.size(); ++i) {
        if (!validate_message_symbol_names(
                file, file.file.message_type[i],
                file_message_path(static_cast<int>(i)), prefix, error)) {
            return false;
        }
    }
    for (std::size_t i = 0; i < file.file.service.size(); ++i) {
        const ServiceDescriptorProto& service = file.file.service[i];
        const DescriptorPath service_path = file_service_path(static_cast<int>(i));
        if (!check_symbol_name(file, service.name, "service", prefix,
                               service_path, error)) {
            return false;
        }
        // D-57 (fix round r25): a method declaration is a declaration name and
        // protoc checks it like any other - `ValidateSymbolName` for the name
        // (descriptor.cc:7208/7228) and the service's own scope for a duplicate
        // (`"go" is already defined in "p.S".`). The decoder used to drop the
        // field entirely, so neither shape was ever seen. The names live only
        // here: a method is not a type and is deliberately not indexed, so the
        // duplicate is answered by this local set rather than by `add_symbol`.
        const std::string service_scope = joined_name(prefix, service.name);
        // Views, not owned strings: the set only has to answer "seen this name
        // before", and the names live in the descriptor this function owns, so
        // a node per method is enough - the baseline also stored a std::string
        // copy inside every node.
        std::set<str_view> method_names;
        for (std::size_t j = 0; j < service.method.size(); ++j) {
            const str_view method_name = service.method[j].name;
            if (!is_valid_symbol_name(method_name)) {
                return fail_symbol_name(
                    file, method_name, "method", service_scope,
                    service_method_path(service_path, static_cast<int>(j)),
                    error);
            }
            if (!method_names.insert(method_name).second) {
                const DescriptorPath method_path =
                    service_method_path(service_path, static_cast<int>(j));
                return fail_at_code(
                    file,
                    "duplicate method name \"" + std::string(method_name) +
                            "\" in " + service_scope,
                    declaration_location(file, method_path), method_path,
                    DIAGNOSTIC_DUPLICATE_SYMBOL, error);
            }
        }
    }
    return true;
}

bool index_file(SchemaFile& file, SymbolIndex& symbols, Diagnostic& error)
{
    // Pass 1: validate every lexical scope before resolving anything, so
    // colliding declarations fail even when no field references them.
    const std::string prefix = package_prefix(file);
    if (!validate_file_scope_names(file, prefix, error)) return false;
    for (std::size_t i = 0; i < file.file.message_type.size(); ++i) {
        if (!validate_message_scope_names(file, file.file.message_type[i],
                                          file_message_path(static_cast<int>(i)),
                                          prefix, error)) {
            return false;
        }
    }
    // Pass 2: build the complete table before resolving any field, so
    // forward references and mutually-referential messages work.
    if (file.file.has_package && !file.file.package.empty()) {
        if (!index_package_scopes(file, symbols, view_text(file.file.package),
                                  error)) {
            return false;
        }
    }
    for (std::size_t i = 0; i < file.file.enum_type.size(); ++i) {
        const DescriptorPath enum_path = file_enum_path(static_cast<int>(i));
        const std::string enum_name =
            joined_name(prefix, file.file.enum_type[i].name);
        if (!add_symbol(file, symbols, enum_name, FieldDescriptorProto::TYPE_ENUM,
                        enum_path, error)) {
            return false;
        }
        for (std::size_t j = 0; j < file.file.enum_type[i].value.size(); ++j) {
            if (!add_symbol(file, symbols,
                            joined_name(prefix, file.file.enum_type[i].value[j].name),
                            0, enum_value_path(enum_path, static_cast<int>(j)),
                            error)) {
                return false;
            }
        }
    }
    for (std::size_t i = 0; i < file.file.service.size(); ++i) {
        if (!add_symbol(file, symbols,
                        joined_name(prefix, file.file.service[i].name),
                        SYMBOL_SERVICE, file_service_path(static_cast<int>(i)),
                        error)) {
            return false;
        }
    }
    for (std::size_t i = 0; i < file.file.message_type.size(); ++i) {
        if (!index_message(file, symbols, file.file.message_type[i], prefix,
                           file_message_path(static_cast<int>(i)), error)) {
            return false;
        }
    }
    return true;
}

bool resolve_file(SchemaFile& file, const SymbolIndex& symbols,
                  const std::set<const SchemaFile*>& visible_files,
                  bool allow_unresolved, Diagnostic& error)
{
    // The exclusive-mode policy: FATAL for a complete link, DEFERRED when the
    // caller is the parser's standalone pass (W-59).
    const UnresolvedPolicy unresolved_policy =
        allow_unresolved ? UnresolvedPolicy::deferred() : UnresolvedPolicy();
    const std::string prefix = package_prefix(file);
    for (std::size_t i = 0; i < file.file.message_type.size(); ++i) {
        if (!resolve_message(file, symbols, visible_files, unresolved_policy,
                             true,
                             file.file.message_type[i], prefix,
                             file_message_path(static_cast<int>(i)), error)) {
            return false;
        }
    }
    // W-16: the service half of the same pass. It runs after every message,
    // so a method may reference any message the file declares.
    return resolve_services(file, symbols, visible_files, unresolved_policy,
                            true, error);
}

bool resolve_file_compat(SchemaFile& file, const SymbolIndex& symbols,
                         const std::set<const SchemaFile*>& visible_files,
                         std::set<std::string>& warned_types,
                         bool& missing_external, Diagnostic& error)
{
    // The named policy of A6: the shared resolver, told what a reference that
    // resolves nowhere means to THIS pass. The per-type dedup set and the
    // missing-information report are the caller's, because both describe the
    // whole link (A5, A4), not one message; the walk is the same declaration
    // order resolve_file uses, so the first failure is deterministic.
    const UnresolvedPolicy unresolved_policy =
        UnresolvedPolicy::external(warned_types, missing_external);
    const std::string prefix = package_prefix(file);
    for (std::size_t i = 0; i < file.file.message_type.size(); ++i) {
        if (!resolve_message(file, symbols, visible_files, unresolved_policy,
                             true,
                             file.file.message_type[i], prefix,
                             file_message_path(static_cast<int>(i)), error)) {
            return false;
        }
    }
    // W-16: a method endpoint follows the same tolerance as a field type - an
    // absolute name the thin set does not carry is an external reference -
    // and the same caller-owned warning bound.
    return resolve_services(file, symbols, visible_files, unresolved_policy,
                            true, error);
}

bool is_deferred_default_warning(const Diagnostic& diagnostic)
{
    // The paired companion of an unresolved-type warning: the parser's
    // standalone pass records it for the same field because it could not
    // validate the default of a type it had not resolved. After a complete link
    // that resolved the type and validated the default, it is stale, so A10
    // drops it together with the unresolved-type warning it was paired with
    // (fix round r4, ledger D-13).
    //
    // The match is (GENERIC, kDeferredDefaultPrefix) - deliberately narrow,
    // and a contract: exactly one producer writes that sentence (the line at
    // kDeferredDefaultPrefix's definition), no other diagnostic may start
    // with it, and a new producer of a droppable warning must extend this
    // predicate and its test instead of reusing the prefix. An unrelated
    // GENERIC warning survives A10 (the pinned control). The match also
    // covers DIAGNOSTIC_UNRESOLVED_TYPE and DIAGNOSTIC_INVISIBLE_TYPE pairs
    // only through their own codes at the drop site; a partial-visible-set
    // caller that leaves stale INVISIBLE warnings behind is D-27's second
    // edge and has no producer today.
    return diagnostic.code == DIAGNOSTIC_GENERIC &&
           diagnostic.message.size() >= kDeferredDefaultPrefix.size() &&
           diagnostic.message.compare(0, kDeferredDefaultPrefix.size(),
                                      kDeferredDefaultPrefix) == 0;
}

bool validate_deferred_file(SchemaFile& file, Diagnostic& error)
{
    // No symbols are consulted and no warnings are produced: every named
    // type stays unresolved for import linking, so only kind-independent
    // checks run here.
    const SymbolIndex no_symbols;
    const std::set<const SchemaFile*> no_visible_files;
    const UnresolvedPolicy unresolved_policy = UnresolvedPolicy::deferred();
    const std::string prefix = package_prefix(file);
    for (std::size_t i = 0; i < file.file.message_type.size(); ++i) {
        if (!resolve_message(file, no_symbols, no_visible_files,
                             unresolved_policy, false,
                             file.file.message_type[i], prefix,
                             file_message_path(static_cast<int>(i)), error)) {
            return false;
        }
    }
    // W-16: the endpoint fields are reference-free shape; the names themselves
    // are not resolved (link_types == false), because a later linking pass is
    // what makes an imported message visible.
    return resolve_services(file, no_symbols, no_visible_files,
                            unresolved_policy, false, error);
}

} // namespace easypb_schema
