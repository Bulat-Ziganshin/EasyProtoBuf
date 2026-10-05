#ifndef EASYPB_SCHEMA_SEMANTICS_INTERNAL_HPP_INCLUDED
#define EASYPB_SCHEMA_SEMANTICS_INTERNAL_HPP_INCLUDED

// Internal seam for the semantic rule helpers that `resolve_message` used to
// inline (P1 split). The public surface is schema_semantics.hpp; nothing here
// is installed or part of it. Declaring these rules under a named namespace
// instead of the anonymous one is what lets the unit suite call one rule in
// isolation, without walking a whole message subtree: the previous anonymous
// linkage made each rule reachable only through resolve_file,
// resolve_file_compat or validate_deferred_file.
//
// The seam is deliberately narrow. Only the declarations move out of
// schema_semantics.cpp; the definitions stay there, so the two cannot drift -
// a changed definition is checked against the declaration in the same
// translation unit. Everything else in that file stays anonymous.

#include <set>
#include <string>

#include "schema_semantics.hpp"

namespace easypb_schema {
namespace internal {

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
                                   bool is_proto3, Diagnostic& error);

// 2. type resolution - only when link_types; reports the two outcome flags
bool resolve_field_type(SchemaFile& file, const SymbolIndex& symbols,
                        const std::set<const SchemaFile*>& visible_files,
                        const UnresolvedPolicy& unresolved_policy,
                        const std::string& scope, bool link_types,
                        FieldDescriptorProto& field, const DescriptorPath& field_path,
                        bool* unknown, bool* external_reference,
                        Diagnostic& error);

// 3. the field rules that run after resolution - packed legality, the oneof
// index and label checks, and default validation
bool validate_field_encoding(SchemaFile& file, const SymbolIndex& symbols,
                             const std::set<const SchemaFile*>& visible_files,
                             const std::string& scope,
                             const DescriptorProto& message, bool link_types,
                             FieldDescriptorProto& field, const DescriptorPath& field_path,
                             bool unknown, bool external_reference,
                             Diagnostic& error);

// The type-dependent validator behind rule 3's default dispatch (check 12):
// exposed separately so one scalar or named default can be probed without a
// field-encoding fixture. It is the only anonymous helper this seam reaches;
// the other large validators - validate_symbol_names,
// validate_enum_value_numbers and validate_map_entry_shapes - are already
// public in schema_semantics.hpp and need no seam.
bool validate_default(SchemaFile& file, const SymbolIndex& symbols,
                      const std::set<const SchemaFile*>& visible_files,
                      const FieldDescriptorProto& field, bool unknown,
                      const DescriptorPath& field_path, Diagnostic& error);

// 4. oneof declarations claimed by no field (D-28) - after the field loop on
// purpose
bool validate_message_oneofs(SchemaFile& file, const DescriptorProto& message,
                             const std::string& scope,
                             const DescriptorPath& message_path, Diagnostic& error);

// 5. nested type names, then recursion
bool validate_nested_types(SchemaFile& file, const SymbolIndex& symbols,
                           const std::set<const SchemaFile*>& visible_files,
                           const UnresolvedPolicy& unresolved_policy, bool link_types,
                           DescriptorProto& message, const std::string& scope,
                           const DescriptorPath& message_path, Diagnostic& error);

} // namespace internal
} // namespace easypb_schema

#endif
