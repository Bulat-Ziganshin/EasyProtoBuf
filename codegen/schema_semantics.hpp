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
// FieldDescriptorProto TYPE_* kind; package scopes use SYMBOL_PACKAGE and
// service names SYMBOL_SERVICE; every other declaration category (enum
// values, fields, oneofs) uses kind 0 because field type names never resolve
// to them. Keys are owned fully qualified names such as ".pkg.Outer.Inner".
// Package scopes merge across files rather than conflict; any other collision
// is an error.
//
// The service value exists because protoc asks two questions about a service
// and the lookup needs both answers (`descriptor.cc:676-679`):
// `Symbol::IsAggregate()` counts a service, so a service may COMMIT the head
// of a compound relative name (and then fails the member lookup, having no
// members); `Symbol::IsType()` does not, so a service may never win a
// whole-name reference. Appended, never inserted: `kind` is compared against
// the FieldDescriptorProto TYPE_* values, whose numbers must keep their
// identity (fix round r10, `D-21`).
enum SymbolKind {
    SYMBOL_PACKAGE = 100,
    SYMBOL_SERVICE = 101
};
struct SymbolEntry
{
    int kind;
    // Borrows its SchemaFile from the indexed SchemaSet: the set must
    // outlive any index referencing it and must not be cleared or
    // destroyed while referenced (same rule as the file-to-file
    // references in schema.hpp). Growth is safe: files live on the heap,
    // so their addresses stay stable while the owning vector grows.
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
// on conflicting declarations, reported as DIAGNOSTIC_DUPLICATE_SYMBOL with
// both declarations named. The code is the same for both producers - a name
// already taken inside this file's own scopes, which the scope validation
// catches before anything is indexed and which names both declarations by
// kind within the scope, and a name taken by an already indexed file, which
// additionally names both logical file names - so no caller has to read a
// message text to tell a redeclaration from a generic failure (E1/A3).
// error.file is the file being indexed. Package scopes merge instead of
// conflicting. Each file must be indexed once: re-indexing the same file
// reports its own declarations as duplicates.
bool index_file(SchemaFile& file, SymbolIndex& symbols, Diagnostic& error);

// Resolves custom field type names against symbols, considering only files
// in visible_files, and applies type-dependent validation (packed legality,
// oneof bounds, default values). A reference the index does not know at all
// is reported once as DIAGNOSTIC_UNRESOLVED_TYPE when allow_unresolved is
// true and as an error when it is false; an invisible declared type reports
// DIAGNOSTIC_INVISIBLE_TYPE the same way, and a found non-type or a foreign
// map entry is fatal in BOTH modes - no import can turn it into a legal
// field type (the B9/B15 oracle rows). Checks that do not need the resolved
// kind are always applied. Returns false and fills error on the first fatal
// problem.
bool resolve_file(SchemaFile& file, const SymbolIndex& symbols,
                  const std::set<const SchemaFile*>& visible_files,
                  bool allow_unresolved, Diagnostic& error);

// The compatibility finish pass (plan 4 Task 3, amendments A5/A6): the one
// descriptor-origin file of LINK_EXTERNAL_DESCRIPTOR_COMPATIBILITY is resolved
// against its own declarations, and a reference that resolves nowhere is
// tolerated ONLY when the descriptor classifies it itself - an explicit
// TYPE_MESSAGE/TYPE_ENUM kind and an absolute type name. Every permitted
// external reference records one warning per type name: the bound is the
// caller-owned warned_types set (A5), while warn_once only keeps a repeated
// pass idempotent, and the warning goes to the stable unknown location because
// a thin descriptor carries no positions. missing_external is the pass's own
// report that external information was absent (A4), and a field whose type
// resolves locally is checked exactly as complete linking checks it - the
// declared-kind comparison (A7) included. Unlike resolve_file's
// allow_unresolved policy, this pass never accepts a relative source-style
// name, and it never rewrites the metadata of a permitted external reference
// (a refused reference keeps its metadata too, because every check runs before
// the rewrite). Returns false and fills error on the first fatal problem.
bool resolve_file_compat(SchemaFile& file, const SymbolIndex& symbols,
                         const std::set<const SchemaFile*>& visible_files,
                         std::set<std::string>& warned_types,
                         bool& missing_external, Diagnostic& error);

// Validates the shape of every synthetic map entry the file declares
// (amendment A26): a message carrying options.map_entry == true must hold a
// field numbered 1 and a field numbered 2, referenced or not, reported as
// DIAGNOSTIC_MALFORMED_MAP_ENTRY with the message text the generator prints
// today. An entry nobody references is still descriptor input, so the check is
// unconditional and must not hide behind the map-entry ownership guard, which
// only fires for a referencing field. Source origin cannot produce the shape
// (the parser synthesizes both members), so this is a descriptor-origin check:
// `link_schema` runs it in complete mode, and compatibility mode's own sweep is
// Step 7's obligation, not this function's (review r4, style-only note).
// Returns false and fills error at the first malformed entry, in declaration
// order.
bool validate_map_entry_shapes(SchemaFile& file, Diagnostic& error);

// True for the paired companion of an unresolved-type warning: the generic
// warning the standalone pass records when it cannot validate the default of a
// type it has not resolved. Complete linking resolves the type and validates the
// default, which makes it stale, so A10's warning hygiene removes it together
// with the unresolved-type warning it was recorded with. The sentence itself
// stays private to schema_semantics.cpp - callers ask this question instead of
// matching on message text (fix round r4, ledger D-13). The match is a
// contract, not a convenience: exactly one producer writes that sentence, no
// other diagnostic may start with it, and a new droppable warning extends
// this predicate and its test instead of reusing the prefix (ledger D-27,
// fix round r12).
bool is_deferred_default_warning(const Diagnostic& diagnostic);

// Validates everything that needs no import linking: duplicate field
// names/numbers, oneof bounds, packed labels, and scalar packed/default
// rules. Named-type packed/default checks are left for complete linking,
// when shadowing imports may change the resolved kind. Emits no warnings
// and resolves nothing; every named type stays raw with has_type == false.
bool validate_deferred_file(SchemaFile& file, Diagnostic& error);

} // namespace easypb_schema

#endif
