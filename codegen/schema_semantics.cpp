#include "schema_semantics.hpp"

#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <sstream>

namespace easypb_schema {

namespace {

std::string view_text(const str_view& value)
{
    return std::string(value.data(), value.size());
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

bool is_float_text(const std::string& text)
{
    std::size_t start = 0;
    if (!text.empty() && (text[0] == '+' || text[0] == '-')) start = 1;
    const std::string body = text.substr(start);
    if (body == "inf" || body == "nan") return true;
    if (body.empty()) return false;
    char* end = 0;
    errno = 0;
    (void)std::strtod(text.c_str(), &end);
    return end != text.c_str() && *end == '\0' && errno != ERANGE;
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
bool add_scope_name(const SchemaFile& file,
                    std::map<std::string, std::string>& names,
                    const std::string& name,
                    const std::string& kind,
                    const std::string& scope,
                    const DescriptorPath& path,
                    Diagnostic& error)
{
    const std::pair<std::map<std::string, std::string>::iterator, bool> inserted =
        names.insert(std::make_pair(name, kind));
    if (!inserted.second) {
        std::string message = "name collision in " +
            (scope.empty() ? std::string("global scope") : scope) +
            ": " + kind + " " + name + " conflicts with " +
            inserted.first->second;
        SourceLocation location = declaration_location(file, path);
        if (location.line == 0) message += " at " + format_path(path);
        error.file = view_text(file.file.name);
        error.location = location;
        error.message = message;
        error.warning = false;
        error.code = DIAGNOSTIC_DUPLICATE_SYMBOL;
        return false;
    }
    return true;
}

bool validate_file_scope_names(const SchemaFile& file, const std::string& scope,
                               Diagnostic& error)
{
    std::map<std::string, std::string> names;
    for (std::size_t i = 0; i < file.file.enum_type.size(); ++i) {
        if (!add_scope_name(file, names, view_text(file.file.enum_type[i].name),
                            "enum type", scope, file_enum_path(static_cast<int>(i)),
                            error)) {
            return false;
        }
    }
    for (std::size_t i = 0; i < file.file.message_type.size(); ++i) {
        if (!add_scope_name(file, names, view_text(file.file.message_type[i].name),
                            "message", scope, file_message_path(static_cast<int>(i)),
                            error)) {
            return false;
        }
    }
    for (std::size_t i = 0; i < file.file.service.size(); ++i) {
        if (!add_scope_name(file, names, view_text(file.file.service[i].name),
                            "service", scope, file_service_path(static_cast<int>(i)),
                            error)) {
            return false;
        }
    }
    for (std::size_t i = 0; i < file.file.enum_type.size(); ++i) {
        const DescriptorPath enum_path = file_enum_path(static_cast<int>(i));
        for (std::size_t j = 0; j < file.file.enum_type[i].value.size(); ++j) {
            if (!add_scope_name(file, names,
                                view_text(file.file.enum_type[i].value[j].name),
                                "enum value", scope,
                                enum_value_path(enum_path, static_cast<int>(j)),
                                error)) {
                return false;
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
    const std::string scope = parent + "." + view_text(message.name);
    std::map<std::string, std::string> names;
    for (std::size_t i = 0; i < message.field.size(); ++i) {
        if (!add_scope_name(file, names, view_text(message.field[i].name),
                            "field", scope,
                            message_field_path(message_path, static_cast<int>(i)),
                            error)) {
            return false;
        }
    }
    for (std::size_t i = 0; i < message.nested_type.size(); ++i) {
        if (!add_scope_name(file, names, view_text(message.nested_type[i].name),
                            "nested message", scope,
                            message_nested_path(message_path, static_cast<int>(i)),
                            error)) {
            return false;
        }
    }
    for (std::size_t i = 0; i < message.enum_type.size(); ++i) {
        if (!add_scope_name(file, names, view_text(message.enum_type[i].name),
                            "nested enum type", scope,
                            message_enum_path(message_path, static_cast<int>(i)),
                            error)) {
            return false;
        }
    }
    for (std::size_t i = 0; i < message.oneof_decl.size(); ++i) {
        if (!add_scope_name(file, names, view_text(message.oneof_decl[i].name),
                            "oneof", scope,
                            message_oneof_path(message_path, static_cast<int>(i)),
                            error)) {
            return false;
        }
    }
    for (std::size_t i = 0; i < message.enum_type.size(); ++i) {
        const DescriptorPath enum_path =
            message_enum_path(message_path, static_cast<int>(i));
        for (std::size_t j = 0; j < message.enum_type[i].value.size(); ++j) {
            if (!add_scope_name(file, names,
                                view_text(message.enum_type[i].value[j].name),
                                "enum value", scope,
                                enum_value_path(enum_path, static_cast<int>(j)),
                                error)) {
                return false;
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
    const std::pair<std::map<std::string, SymbolEntry>::iterator, bool> inserted =
        symbols.symbols.insert(std::make_pair(name, entry));
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
        std::string text = message + " in \"" + view_text(file.file.name) + "\"";
        if (location.line == 0 && !path.empty()) {
            text += " at " + format_path(path);
        }
        text += "; already defined in \"" + view_text(previous->file.name) + "\"";
        const SourceLocation previous_location =
            declaration_location(*previous, inserted.first->second.path);
        if (previous_location.line == 0 && !inserted.first->second.path.empty()) {
            text += " at " + format_path(inserted.first->second.path);
        }
        error.file = view_text(file.file.name);
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
    const std::string name = parent + "." + view_text(message.name);
    if (!add_symbol(file, symbols, name, FieldDescriptorProto::TYPE_MESSAGE,
                    message_path, error)) {
        return false;
    }
    for (std::size_t i = 0; i < message.field.size(); ++i) {
        if (!add_symbol(file, symbols, name + "." + view_text(message.field[i].name),
                        0, message_field_path(message_path, static_cast<int>(i)),
                        error)) {
            return false;
        }
    }
    for (std::size_t i = 0; i < message.oneof_decl.size(); ++i) {
        if (!add_symbol(file, symbols,
                        name + "." + view_text(message.oneof_decl[i].name),
                        0, message_oneof_path(message_path, static_cast<int>(i)),
                        error)) {
            return false;
        }
    }
    for (std::size_t i = 0; i < message.enum_type.size(); ++i) {
        const DescriptorPath enum_path =
            message_enum_path(message_path, static_cast<int>(i));
        const std::string enum_name =
            name + "." + view_text(message.enum_type[i].name);
        if (!add_symbol(file, symbols, enum_name, FieldDescriptorProto::TYPE_ENUM,
                        enum_path, error)) {
            return false;
        }
        for (std::size_t j = 0; j < message.enum_type[i].value.size(); ++j) {
            if (!add_symbol(file, symbols,
                            name + "." + view_text(message.enum_type[i].value[j].name),
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

// The field-number bounds both frontends enforce. The parser refuses these
// shapes as source text (proto_parser.cpp:1049-1060: 0, above the maximum,
// and the reserved range) and resolve_message refuses them on
// descriptor-origin input, so the two halves of the frontend parity rule
// (contract clause 12, D-23) watch the same numbers.
const std::int32_t kMaxFieldNumber = 536870911;
const std::int32_t kReservedFieldNumberFirst = 19000;
const std::int32_t kReservedFieldNumberLast = 19999;

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
    for (std::set<const SchemaFile*>::const_iterator it = visible_files.begin();
         it != visible_files.end(); ++it) {
        const std::string declared = package_prefix(**it);
        if (declared == package_head) return true;
        if (declared.size() > package_head.size() &&
            declared.compare(0, package_head.size(), package_head) == 0 &&
            declared[package_head.size()] == '.') {
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

// protoc's ownership rule for a synthetic map entry (A22, measured): the
// reference belongs to the owning map field exactly when the entry is nested
// directly in the field's own message, the field is repeated, and the field's
// own NAME derives the entry's simple name through the shared map_entry_name
// (A24). The ownership test never looks at the raw reference text, which
// differs by origin (relative for source, absolute for descriptors).
//
// The entry's own map_entry marker proves the message kind, and that is the
// only kind test that survives deferred resolution: a source map field carries
// no assigned kind until linking, so requiring TYPE_MESSAGE here would reject
// every legal source map field the loader deferred.
bool owns_map_entry(const ResolveContext& context, const SymbolEntry& entry)
{
    if (context.field == 0 || context.owner == 0 || context.field_path == 0) {
        return false;
    }
    if (entry.owner != context.owner) return false;
    if (context.field->label != FieldDescriptorProto::LABEL_REPEATED) return false;
    const DescriptorProto* entry_message = find_message(*entry.owner, entry.path);
    if (entry_message == 0) return false;
    if (!entry_message->options.has_map_entry || !entry_message->options.map_entry) {
        return false;
    }
    if (!nested_in_field_message(context, entry.path)) return false;
    return map_entry_name(view_text(context.field->name)) ==
           view_text(entry_message->name);
}

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
    return !owns_map_entry(context, entry);
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
    diagnostic.file = view_text(file.file.name);
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
    if (location.line == 0) text += " at " + format_path(path);
    error.file = view_text(file.file.name);
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
bool is_valid_symbol_name(const std::string& name)
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

// The one site that turns a malformed declaration name into a diagnostic, so
// every kind reports the same category with the same code (E1/A3). The message
// mirrors protoc's two wordings - an empty name is "missing", an illegal
// character is "not a valid identifier" - while the code stays DIAGNOSTIC_
// INVALID_SYMBOL_NAME for both, because they are one category: the name is not a
// symbol name. `scope` names the containing scope in the message; the location
// and descriptor path are the declaration's own.
bool check_symbol_name(const SchemaFile& file, const std::string& name,
                       const std::string& kind, const std::string& scope,
                       const DescriptorPath& path, Diagnostic& error)
{
    if (is_valid_symbol_name(name)) return true;
    const std::string where =
        scope.empty() ? std::string("the global scope") : scope;
    std::string message;
    if (name.empty()) {
        message = kind + " in " + where + " has no name";
    } else {
        message = kind + " name \"" + name +
                  "\" in " + where + " is not a valid identifier";
    }
    return fail_at_code(file, message, declaration_location(file, path), path,
                        DIAGNOSTIC_INVALID_SYMBOL_NAME, error);
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
    const std::string scope = parent + "." + view_text(message.name);
    if (!check_symbol_name(file, view_text(message.name), "message", parent,
                           message_path, error)) {
        return false;
    }
    for (std::size_t i = 0; i < message.field.size(); ++i) {
        if (!check_symbol_name(
                file, view_text(message.field[i].name), "field", scope,
                message_field_path(message_path, static_cast<int>(i)), error)) {
            return false;
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
        const DescriptorPath enum_path =
            message_enum_path(message_path, static_cast<int>(i));
        if (!check_symbol_name(file, view_text(message.enum_type[i].name),
                               "nested enum type", scope, enum_path, error)) {
            return false;
        }
        for (std::size_t j = 0; j < message.enum_type[i].value.size(); ++j) {
            if (!check_symbol_name(
                    file, view_text(message.enum_type[i].value[j].name),
                    "enum value", scope,
                    enum_value_path(enum_path, static_cast<int>(j)), error)) {
                return false;
            }
        }
    }
    for (std::size_t i = 0; i < message.oneof_decl.size(); ++i) {
        if (!check_symbol_name(
                file, view_text(message.oneof_decl[i].name), "oneof", scope,
                message_oneof_path(message_path, static_cast<int>(i)), error)) {
            return false;
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
    error.file = view_text(file.file.name);
    error.location = unknown_location();
    error.message = "Malformed map entry " + entry_name +
                    ": fields 1 and 2 are required";
    error.warning = false;
    error.code = DIAGNOSTIC_MALFORMED_MAP_ENTRY;
    return false;
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
        bool has_key = false;
        bool has_value = false;
        for (std::size_t i = 0; i < message.field.size(); ++i) {
            if (message.field[i].number == 1) has_key = true;
            if (message.field[i].number == 2) has_value = true;
        }
        if (!has_key || !has_value) {
            return fail_malformed_map_entry(file, qualified_name, error);
        }
    }
    for (std::size_t i = 0; i < message.nested_type.size(); ++i) {
        if (!validate_message_map_entries(
                file, message.nested_type[i],
                qualified_name + "." + view_text(message.nested_type[i].name),
                error)) {
            return false;
        }
    }
    return true;
}

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
// in the other direction.
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
    if (enumeration.options.has_allow_alias &&
        enumeration.options.allow_alias) {
        return true;
    }
    std::map<std::int32_t, std::string> first_by_number;
    for (std::size_t i = 0; i < enumeration.value.size(); ++i) {
        const std::int32_t number = enumeration.value[i].number;
        const std::string value_name = view_text(enumeration.value[i].name);
        const std::map<std::int32_t, std::string>::const_iterator seen =
            first_by_number.find(number);
        if (seen != first_by_number.end()) {
            const DescriptorPath value_path =
                enum_value_path(enum_path, static_cast<int>(i));
            return fail_at_code(
                file,
                "enum value \"" + value_name + "\" of \"" + enum_name +
                    "\" repeats number " + number_text(number) +
                    ", which \"" + seen->second +
                    "\" already uses; set option allow_alias = true to allow "
                    "aliases",
                declaration_location(file, value_path), value_path,
                DIAGNOSTIC_DUPLICATE_ENUM_NUMBER, error);
        }
        first_by_number.insert(std::make_pair(number, value_name));
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
                qualified_name + "." + view_text(message.enum_type[i].name),
                message_enum_path(message_path, static_cast<int>(i)), error)) {
            return false;
        }
    }
    for (std::size_t i = 0; i < message.nested_type.size(); ++i) {
        if (!validate_message_enum_value_numbers(
                file, message.nested_type[i],
                qualified_name + "." + view_text(message.nested_type[i].name),
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
            message += " is not visible from \"" + view_text(file.file.name) + "\"";
        } else {
            message += " is defined in \"" + defining + "\", which is not visible "
                       "from \"" + view_text(file.file.name) + "\"";
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

// Type-dependent semantic validation for the textual default_value stored
// during parsing. Unknown kinds are the caller's responsibility: the caller
// passes unknown == true when the named type could not be resolved, in which
// case validation is deferred with a warning instead of incorrectly
// rejecting an imported packed enum or an imported enum default.
bool validate_default(SchemaFile& file, const SymbolIndex& symbols,
                      const std::set<const SchemaFile*>& visible_files,
                      const FieldDescriptorProto& field, bool unknown,
                      const DescriptorPath& field_path, Diagnostic& error)
{
    if (!field.has_default_value) return true;
    const std::string value = view_text(field.default_value);
    std::int32_t signed32 = 0;
    std::int64_t signed64 = 0;
    const SourceLocation where = field_default_location(file, field_path);
    // An unresolvable named type may be an imported enum (whose default is
    // then legal) or an imported message (whose default complete linking
    // will reject). Either way validation is deferred with a warning instead
    // of failing on the unknown kind. Deferred parsing leaves type == 0, so
    // this check must precede the switch below.
    if (unknown) {
        warn_once(file,
                  kDeferredDefaultPrefix + view_text(field.type_name),
                  DIAGNOSTIC_GENERIC, where, field_path);
        return true;
    }
    switch (field.type) {
        case FieldDescriptorProto::TYPE_STRING:
        case FieldDescriptorProto::TYPE_BYTES:
            return true;
        case FieldDescriptorProto::TYPE_BOOL:
            if (value != "true" && value != "false") {
                return fail_at(file, "bool default must be true or false",
                               where, field_path, error);
            }
            return true;
        case FieldDescriptorProto::TYPE_FLOAT:
        case FieldDescriptorProto::TYPE_DOUBLE:
            if (!is_float_text(value)) {
                return fail_at(file, "invalid floating-point default",
                               where, field_path, error);
            }
            return true;
        case FieldDescriptorProto::TYPE_ENUM:
            if (value.empty()) {
                return fail_at(file, "enum default must name an enum value",
                               where, field_path, error);
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
                                   where, field_path, error);
                }
                for (std::size_t i = 0; i < enumeration->value.size(); ++i) {
                    if (view_text(enumeration->value[i].name) == value) return true;
                }
                return fail_at(file,
                               "enum type " + enum_name + " has no value named " + value,
                               where, field_path, error);
            }
        case FieldDescriptorProto::TYPE_MESSAGE:
            return fail_at(file, "message fields cannot have defaults",
                           where, field_path, error);
        case FieldDescriptorProto::TYPE_INT32:
        case FieldDescriptorProto::TYPE_SINT32:
        case FieldDescriptorProto::TYPE_SFIXED32:
            if (!parse_signed_32(value, signed32)) {
                return fail_at(file, "default does not fit signed 32-bit field",
                               where, field_path, error);
            }
            return true;
        case FieldDescriptorProto::TYPE_UINT32:
        case FieldDescriptorProto::TYPE_FIXED32:
            if (!parse_unsigned_with_optional_plus(value, 4294967295ull)) {
                return fail_at(file, "default does not fit unsigned 32-bit field",
                               where, field_path, error);
            }
            return true;
        case FieldDescriptorProto::TYPE_INT64:
        case FieldDescriptorProto::TYPE_SINT64:
        case FieldDescriptorProto::TYPE_SFIXED64:
            if (!parse_signed_64(value, signed64)) {
                return fail_at(file, "default does not fit signed 64-bit field",
                               where, field_path, error);
            }
            return true;
        case FieldDescriptorProto::TYPE_UINT64:
        case FieldDescriptorProto::TYPE_FIXED64:
            if (!parse_unsigned_with_optional_plus(value, 18446744073709551615ull)) {
                return fail_at(file, "default does not fit unsigned 64-bit field",
                               where, field_path, error);
            }
            return true;
        default:
            return fail_at(file, "unsupported field type in default validation",
                           where, field_path, error);
    }
}

// What a named type that resolves nowhere means to the pass that called
// resolve_message. Exactly one of the three behaviours applies, and the two
// pointers are the compatibility pass's caller-owned state (A4/A5):
//
// - complete linking: fatal, with the shared outcome code (complete);
// - the parser's standalone pass: a per-field warning that keeps the raw
//   metadata so a later complete link can resolve it (deferred);
// - descriptor compatibility: an external reference is permitted only when
//   the descriptor classifies it itself, and its warning is bounded one per
//   type name by the caller-owned set (external_warned_types non-null).
struct UnresolvedPolicy
{
    bool deferred;
    std::set<std::string>* external_warned_types;
    bool* missing_external;

    UnresolvedPolicy()
        : deferred(false), external_warned_types(0), missing_external(0) {}
};

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
    const std::string scope = parent + "." + view_text(message.name);
    std::set<std::string> names;
    std::set<std::int32_t> numbers;
    for (std::size_t i = 0; i < message.field.size(); ++i) {
        FieldDescriptorProto& field = message.field[i];
        const DescriptorPath field_path =
            message_field_path(message_path, static_cast<int>(i));
        if (!names.insert(view_text(field.name)).second) {
            return fail_at(file, "duplicate field name " + view_text(field.name),
                           declaration_location(file, field_path), field_path, error);
        }
        if (!numbers.insert(field.number).second) {
            return fail_at(file, "duplicate field number in message " + scope,
                           declaration_location(file, field_path), field_path, error);
        }

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
        // codegen/parser/proto_parser.cpp:1233, and "explicit default values
        // are not allowed in proto3", :1124), so this is the descriptor-origin
        // half of that frontend parity rule. The gate is the field's OWN file:
        // proto2 gives both shapes a meaning, so reading the wrong file here
        // would refuse legal input (the proto2 controls in
        // test_descriptor_origin_proto3_field_rules_are_refused).
        //
        // The open-enum first-value rule is the same family and NOT here: the
        // contract sweep's row B6 assigns open-enum semantics to plan 2/5, and
        // ledger `D-33` records that scope decision (rejected), re-raised by
        // the same review rather than reversed by this round.
        if (classify_file_syntax(file.file) == FILE_SYNTAX_PROTO3) {
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
        }

        bool unknown = false;
        // Set when the compatibility policy permitted an external reference:
        // the declared metadata is kept verbatim and the field's default is
        // not validated, because the external definition cannot answer and the
        // A5 warning already states that defaults were not validated.
        bool external_reference = false;
        if (field.has_type_name) {
            if (!link_types) {
                // Deferred: never resolve locally. The resolved kind is
                // unknown until import linking, even when a local
                // declaration could match today.
                unknown = true;
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
                    const bool declared_group =
                        field.type == FieldDescriptorProto::TYPE_GROUP &&
                        resolved->kind == FieldDescriptorProto::TYPE_MESSAGE;
                    if (file.from_descriptor_set && field.has_type &&
                        !declared_group && field.type != resolved->kind) {
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
                    // Authoritative metadata is not rewritten: a group keeps
                    // its declared TYPE_GROUP, because overwriting it with
                    // TYPE_MESSAGE would silently change the wire encoding the
                    // generator emits for it.
                    field.type = declared_group
                        ? FieldDescriptorProto::TYPE_GROUP
                        : resolved->kind;
                    field.has_type = true;
                    field.type_name = file.strings.save(resolved_name);
                } else {
                    unknown = true;
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
                    if (unresolved_policy.external_warned_types != 0 &&
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
                            !view_text(field.type_name).empty() &&
                            view_text(field.type_name)[0] == '.';
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
                        external_reference = true;
                        if (unresolved_policy.missing_external != 0) {
                            *unresolved_policy.missing_external = true;
                        }
                        const std::string type_name = view_text(field.type_name);
                        if (unresolved_policy.external_warned_types
                                ->insert(type_name).second) {
                            warn_once(file, external_reference_message(type_name),
                                      DIAGNOSTIC_GENERIC, unknown_location(),
                                      field_path);
                        }
                    } else if (unresolved_policy.deferred) {
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

        // Repeatedness is known without resolution: no import can turn an
        // optional field into a repeated one, so reject packed on
        // non-repeated fields immediately. Whether the type itself admits
        // packed encoding is checked only for known kinds.
        if (field.options.has_packed) {
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
        // Scalar kinds are known without linking; named kinds are validated
        // only when linking (link_types) so a shadowed local match cannot
        // decide packed/default validity ahead of import linking.
        if (!field.has_type_name) {
            if (!validate_default(file, symbols, visible_files, field, false,
                                  field_path, error)) {
                return false;
            }
        } else if (link_types && !external_reference) {
            if (!validate_default(file, symbols, visible_files, field, unknown,
                                  field_path, error)) {
                return false;
            }
        }
    }

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

    std::set<std::string> nested_names;
    for (std::size_t i = 0; i < message.enum_type.size(); ++i) {
        const DescriptorPath enum_path =
            message_enum_path(message_path, static_cast<int>(i));
        if (!nested_names.insert(view_text(message.enum_type[i].name)).second) {
            return fail_at(file, "duplicate nested type name in " + scope,
                           declaration_location(file, enum_path),
                           enum_path, error);
        }
    }
    for (std::size_t i = 0; i < message.nested_type.size(); ++i) {
        const DescriptorPath nested_path =
            message_nested_path(message_path, static_cast<int>(i));
        if (!nested_names.insert(view_text(message.nested_type[i].name)).second) {
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

} // namespace

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
                prefix + "." + view_text(file.file.message_type[i].name),
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
                prefix + "." + view_text(file.file.enum_type[i].name),
                file_enum_path(static_cast<int>(i)), error)) {
            return false;
        }
    }
    for (std::size_t i = 0; i < file.file.message_type.size(); ++i) {
        if (!validate_message_enum_value_numbers(
                file, file.file.message_type[i],
                prefix + "." + view_text(file.file.message_type[i].name),
                file_message_path(static_cast<int>(i)), error)) {
            return false;
        }
    }
    return true;
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
    const std::string package = view_text(file.file.package);
    if (file.file.has_package && !package.empty()) {
        std::size_t start = 0;
        for (;;) {
            const std::size_t dot = package.find('.', start);
            const std::string component =
                package.substr(start, (dot == std::string::npos
                                           ? package.size()
                                           : dot) - start);
            if (!check_symbol_name(file, component, "package component",
                                   std::string(), DescriptorPath(), error)) {
                return false;
            }
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
    }
    for (std::size_t i = 0; i < file.file.enum_type.size(); ++i) {
        const DescriptorPath enum_path = file_enum_path(static_cast<int>(i));
        if (!check_symbol_name(file, view_text(file.file.enum_type[i].name),
                               "enum type", prefix, enum_path, error)) {
            return false;
        }
        for (std::size_t j = 0; j < file.file.enum_type[i].value.size(); ++j) {
            if (!check_symbol_name(
                    file, view_text(file.file.enum_type[i].value[j].name),
                    "enum value", prefix,
                    enum_value_path(enum_path, static_cast<int>(j)), error)) {
                return false;
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
        if (!check_symbol_name(file, view_text(file.file.service[i].name),
                               "service", prefix,
                               file_service_path(static_cast<int>(i)), error)) {
            return false;
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
            prefix + "." + view_text(file.file.enum_type[i].name);
        if (!add_symbol(file, symbols, enum_name, FieldDescriptorProto::TYPE_ENUM,
                        enum_path, error)) {
            return false;
        }
        for (std::size_t j = 0; j < file.file.enum_type[i].value.size(); ++j) {
            if (!add_symbol(file, symbols,
                            prefix + "." +
                            view_text(file.file.enum_type[i].value[j].name),
                            0, enum_value_path(enum_path, static_cast<int>(j)),
                            error)) {
                return false;
            }
        }
    }
    for (std::size_t i = 0; i < file.file.service.size(); ++i) {
        if (!add_symbol(file, symbols,
                        prefix + "." + view_text(file.file.service[i].name),
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
    UnresolvedPolicy unresolved_policy;
    unresolved_policy.deferred = allow_unresolved;
    const std::string prefix = package_prefix(file);
    for (std::size_t i = 0; i < file.file.message_type.size(); ++i) {
        if (!resolve_message(file, symbols, visible_files, unresolved_policy,
                             true,
                             file.file.message_type[i], prefix,
                             file_message_path(static_cast<int>(i)), error)) {
            return false;
        }
    }
    return true;
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
    UnresolvedPolicy unresolved_policy;
    unresolved_policy.external_warned_types = &warned_types;
    unresolved_policy.missing_external = &missing_external;
    const std::string prefix = package_prefix(file);
    for (std::size_t i = 0; i < file.file.message_type.size(); ++i) {
        if (!resolve_message(file, symbols, visible_files, unresolved_policy,
                             true,
                             file.file.message_type[i], prefix,
                             file_message_path(static_cast<int>(i)), error)) {
            return false;
        }
    }
    return true;
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
    UnresolvedPolicy unresolved_policy;
    unresolved_policy.deferred = true;
    const std::string prefix = package_prefix(file);
    for (std::size_t i = 0; i < file.file.message_type.size(); ++i) {
        if (!resolve_message(file, no_symbols, no_visible_files,
                             unresolved_policy, false,
                             file.file.message_type[i], prefix,
                             file_message_path(static_cast<int>(i)), error)) {
            return false;
        }
    }
    return true;
}

} // namespace easypb_schema
