#include "schema_semantics.hpp"

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
        error.code = DIAGNOSTIC_GENERIC;
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
        SourceLocation location = declaration_location(file, path);
        if (location.line == 0 && !path.empty()) {
            message += " at " + format_path(path);
        }
        error.file = view_text(file.file.name);
        error.location = location;
        error.message = message;
        error.warning = false;
        error.code = DIAGNOSTIC_GENERIC;
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

// Implements protobuf lexical name lookup over visible type declarations: an
// absolute name is checked directly; a relative name is tried from the
// innermost scope outward to the package/global scope. Non-type declarations
// and declarations owned by invisible files never match. On success the
// matched fully qualified name is stored in *matched_name.
const SymbolEntry* resolve_name(const std::string& raw, const std::string& scope,
                                const SymbolIndex& symbols,
                                const std::set<const SchemaFile*>& visible_files,
                                std::string* matched_name)
{
    if (!raw.empty() && raw[0] == '.') {
        const std::map<std::string, SymbolEntry>::const_iterator found =
            symbols.symbols.find(raw);
        if (found == symbols.symbols.end()) return 0;
        if (found->second.kind != FieldDescriptorProto::TYPE_MESSAGE &&
            found->second.kind != FieldDescriptorProto::TYPE_ENUM) {
            return 0;
        }
        if (visible_files.find(found->second.owner) == visible_files.end()) return 0;
        *matched_name = found->first;
        return &found->second;
    }
    std::string current_scope = scope;
    for (;;) {
        const std::string candidate =
            current_scope.empty() ? "." + raw : current_scope + "." + raw;
        const std::map<std::string, SymbolEntry>::const_iterator found =
            symbols.symbols.find(candidate);
        if (found != symbols.symbols.end() &&
            (found->second.kind == FieldDescriptorProto::TYPE_MESSAGE ||
             found->second.kind == FieldDescriptorProto::TYPE_ENUM) &&
            visible_files.find(found->second.owner) != visible_files.end()) {
            *matched_name = found->first;
            return &found->second;
        }
        if (current_scope.empty()) break;
        const std::size_t dot = current_scope.rfind('.');
        if (dot == std::string::npos || dot == 0) current_scope.clear();
        else current_scope.erase(dot);
    }
    return 0;
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

bool fail_at(const SchemaFile& file, const std::string& message,
             const SourceLocation& location, const DescriptorPath& path,
             Diagnostic& error)
{
    std::string text = message;
    if (location.line == 0) text += " at " + format_path(path);
    error.file = view_text(file.file.name);
    error.location = location;
    error.message = text;
    error.warning = false;
    error.code = DIAGNOSTIC_GENERIC;
    return false;
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
                  "cannot validate default for unresolved imported type " +
                  view_text(field.type_name),
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

// Checks every field in one message subtree. With link_types == true this
// resolves custom field TypeNames and applies the semantic rules that
// require the resolved kind (packed legality for named types, named default
// validation). With link_types == false only checks that need no type
// resolution run: a locally resolvable kind must not decide the outcome,
// because shadowing imports may change it after linking. Unknown-kind
// packed/default rules are then left for complete linking.
bool resolve_message(SchemaFile& file, const SymbolIndex& symbols,
                     const std::set<const SchemaFile*>& visible_files,
                     bool allow_unresolved, bool link_types,
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

        bool unknown = false;
        if (field.has_type_name) {
            if (!link_types) {
                // Deferred: never resolve locally. The resolved kind is
                // unknown until import linking, even when a local
                // declaration could match today.
                unknown = true;
            } else {
                const std::string raw = raw_type_name(file, field, field_path);
                std::string resolved_name;
                const SymbolEntry* resolved =
                    resolve_name(raw, scope, symbols, visible_files, &resolved_name);
                if (resolved != 0) {
                    field.type = resolved->kind;
                    field.has_type = true;
                    field.type_name = file.strings.save(resolved_name);
                } else {
                    unknown = true;
                    if (!allow_unresolved) {
                        return fail_at(file, "unresolved type " + raw + " in " + scope,
                                       field_type_location(file, field_path),
                                       field_path, error);
                    }
                    warn_once(file, "unresolved type " + raw + " in " + scope +
                              "; it is kept as TYPE_MESSAGE until imports are linked",
                              DIAGNOSTIC_UNRESOLVED_TYPE,
                              field_type_location(file, field_path), field_path);
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
        } else if (link_types) {
            if (!validate_default(file, symbols, visible_files, field, unknown,
                                  field_path, error)) {
                return false;
            }
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
        if (!resolve_message(file, symbols, visible_files, allow_unresolved,
                             link_types,
                             message.nested_type[i], scope, nested_path, error)) {
            return false;
        }
    }
    return true;
}

} // namespace

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
                        0, file_service_path(static_cast<int>(i)), error)) {
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
    const std::string prefix = package_prefix(file);
    for (std::size_t i = 0; i < file.file.message_type.size(); ++i) {
        if (!resolve_message(file, symbols, visible_files, allow_unresolved,
                             true,
                             file.file.message_type[i], prefix,
                             file_message_path(static_cast<int>(i)), error)) {
            return false;
        }
    }
    return true;
}

bool validate_deferred_file(SchemaFile& file, Diagnostic& error)
{
    // No symbols are consulted and no warnings are produced: every named
    // type stays unresolved for import linking, so only kind-independent
    // checks run here.
    const SymbolIndex no_symbols;
    const std::set<const SchemaFile*> no_visible_files;
    const std::string prefix = package_prefix(file);
    for (std::size_t i = 0; i < file.file.message_type.size(); ++i) {
        if (!resolve_message(file, no_symbols, no_visible_files, true, false,
                             file.file.message_type[i], prefix,
                             file_message_path(static_cast<int>(i)), error)) {
            return false;
        }
    }
    return true;
}

} // namespace easypb_schema
