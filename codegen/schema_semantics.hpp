#ifndef EASYPB_SCHEMA_SEMANTICS_HPP_INCLUDED
#define EASYPB_SCHEMA_SEMANTICS_HPP_INCLUDED

#include <cstdint>
#include <map>
#include <set>
#include <string>

#include "schema.hpp"

namespace easypb_schema {

// Shared scalar helpers used by both the source parser and the semantic
// passes below. They are moved here instead of being copied so both sides
// classify types and validate scalar defaults identically.
int builtin_type(const std::string& name);
bool packable_type(int type);
bool valid_map_key_type(int type);
bool parse_unsigned_integer(const std::string& text, std::uint64_t& result);
bool parse_signed_32(const std::string& text, std::int32_t& result);
bool is_float_text(const std::string& text);

// One indexed declaration. Message and enum entries use the matching
// FieldDescriptorProto TYPE_* kind; package scopes use SYMBOL_PACKAGE;
// every other declaration category (enum values, fields, oneofs, services)
// uses kind 0 because field type names never resolve to them. Keys are
// owned fully qualified names such as ".pkg.Outer.Inner". Package scopes
// merge across files rather than conflict; any other collision is an error.
enum SymbolKind {
    SYMBOL_PACKAGE = 100
};
struct SymbolEntry
{
    int kind;
    const SchemaFile* owner;
    DescriptorPath path;
};

struct SymbolIndex
{
    std::map<std::string, SymbolEntry> symbols;

    // User-provided so that const SymbolIndex objects can be
    // default-initialized (required since C++11 for const objects;
    // enforced by old Clang releases).
    SymbolIndex() {}
};

// Validates declaration scopes and indexes one file's messages, enums, enum
// values, fields, oneofs, and retained service names. Returns false and
// fills error (with a recorded source position when the file carries one)
// on conflicting declarations. Each file must be indexed once: re-indexing
// the same file reports its own declarations as duplicates.
bool index_file(SchemaFile& file, SymbolIndex& symbols, Diagnostic& error);

// Resolves custom field type names against symbols, considering only files
// in visible_files, and applies type-dependent validation (packed legality,
// oneof bounds, default values). Fields whose type cannot be resolved are
// reported once as DIAGNOSTIC_UNRESOLVED_TYPE warnings when allow_unresolved
// is true, and as errors when it is false. Checks that do not need the
// resolved kind are always applied. Returns false and fills error on the
// first fatal problem.
bool resolve_file(SchemaFile& file, const SymbolIndex& symbols,
                  const std::set<const SchemaFile*>& visible_files,
                  bool allow_unresolved, Diagnostic& error);

// Validates everything that needs no import linking: duplicate field
// names/numbers, oneof bounds, packed labels, and scalar packed/default
// rules. Named-type packed/default checks are left for complete linking,
// when shadowing imports may change the resolved kind. Emits no warnings
// and resolves nothing; every named type stays raw with has_type == false.
bool validate_deferred_file(SchemaFile& file, Diagnostic& error);

} // namespace easypb_schema

#endif
