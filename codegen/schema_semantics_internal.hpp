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
// resolve_message. The three behaviours are exclusive, and this type cannot
// represent a combination: the previous three public flags could, and the
// resolver's external branch silently made a simultaneous deferred request
// unreachable (W-59). The two pointers are the compatibility pass's
// caller-owned state (A4/A5):
//
// - FATAL (the default): complete linking, with the shared outcome code;
// - DEFERRED: the parser's standalone pass, a per-field warning that keeps the
//   raw metadata so a later complete link can resolve it;
// - EXTERNAL: descriptor compatibility, an external reference permitted only
//   when the descriptor classifies it itself, and its warning bounded one per
//   type name by the caller-owned set.
class UnresolvedPolicy
{
public:
    enum Mode { FATAL, DEFERRED, EXTERNAL };

    UnresolvedPolicy()
        : mode_(FATAL), warned_types_(0), missing_external_(0) {}

    static UnresolvedPolicy deferred()
    {
        return UnresolvedPolicy(DEFERRED, 0, 0);
    }

    static UnresolvedPolicy external(std::set<std::string>& warned_types,
                                     bool& missing_external)
    {
        return UnresolvedPolicy(EXTERNAL, &warned_types, &missing_external);
    }

    Mode mode() const { return mode_; }
    std::set<std::string>* warned_types() const { return warned_types_; }
    bool* missing_external() const { return missing_external_; }

private:
    UnresolvedPolicy(Mode mode, std::set<std::string>* warned_types,
                     bool* missing_external)
        : mode_(mode),
          warned_types_(warned_types),
          missing_external_(missing_external)
    {
    }

    Mode mode_;
    std::set<std::string>* warned_types_;
    bool* missing_external_;
};

// The ownership rule for a synthetic map entry (A22, measured): the reference
// belongs to the owning map field exactly when the entry is nested directly in
// the field's own message, the field is repeated, the field's own NAME derives
// the entry's simple name through the shared map_entry_name (A24), and the
// message really carries the map_entry marker. The marker test is a
// PRECONDITION of the rule, not only a test the caller makes (W-49): a second
// caller must not be told that a real nested message is some field's entry.
//
// The five explicit context values replace the anonymous ResolveContext so a
// test can call the rule directly (W-49); the rule is asked only for a
// symbol the index resolved to TYPE_MESSAGE.
bool owns_map_entry(const SchemaFile& owner,
                    const DescriptorPath& field_path,
                    const FieldDescriptorProto& field,
                    const SymbolEntry& entry,
                    const DescriptorProto& entry_message);

// The orchestrator is declared first: validate_nested_types recurses into it.
bool resolve_message(SchemaFile& file, const SymbolIndex& symbols,
                     const std::set<const SchemaFile*>& visible_files,
                     const UnresolvedPolicy& unresolved_policy,
                     bool link_types,
                     DescriptorProto& message,
                     const std::string& parent, const DescriptorPath& message_path,
                     Diagnostic& error);

// The service half of the same walk (`W-16`): both endpoint types of every
// method, after the messages. A missing endpoint field is refused in every
// mode; a name is resolved only when link_types is true.
bool resolve_services(SchemaFile& file, const SymbolIndex& symbols,
                      const std::set<const SchemaFile*>& visible_files,
                      const UnresolvedPolicy& unresolved_policy,
                      bool link_types, Diagnostic& error);

// 1. the shape a field declares - always asked, in both modes
bool validate_field_declared_shape(SchemaFile& file, FieldDescriptorProto& field,
                                   const DescriptorPath& field_path,
                                   bool is_proto3, Diagnostic& error);

// 2. type resolution - only when link_types; reports the two outcome flags
//
// `unknown` and `external_reference` are CALLER-OWNED outputs: both must be
// initialized to false before the call, and the call sets the one that
// describes its outcome and never clears either (W-59). A field with no
// type_name writes neither, so a caller that passes indeterminate storage
// reads garbage on that path.
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
// field-encoding fixture. `unknown` defers an unresolvable named type with a
// warning; `external` says the reference itself was tolerated as external, so
// the kind-decided part of the rule runs while enum membership is deferred to
// the dependency header. Unlike the three message-level validators below,
// validate_default is declared here and DEFINED in `namespace internal` in
// schema_semantics.cpp; it is not an anonymous helper.
bool validate_default(SchemaFile& file, const SymbolIndex& symbols,
                      const std::set<const SchemaFile*>& visible_files,
                      const FieldDescriptorProto& field, bool unknown,
                      bool external, const DescriptorPath& field_path,
                      Diagnostic& error);

// The message-level validators behind the public sweeps (validate_symbol_names,
// validate_enum_value_numbers, validate_map_entry_shapes). They stay anonymous
// in schema_semantics.cpp - they are packed among the helpers that must stay
// anonymous - so they are reached through one-line forwarders near the END of
// that file, after the anonymous rule block closes at schema_semantics.cpp:2151
// (the forwarders themselves sit around :3296-3323). A test can then drive one
// message subtree without a whole-file walk.
bool validate_message_symbol_names(const SchemaFile& file,
                                   const DescriptorProto& message,
                                   const DescriptorPath& message_path,
                                   const std::string& parent,
                                   Diagnostic& error);
bool validate_message_enum_value_numbers(const SchemaFile& file,
                                         const DescriptorProto& message,
                                         const std::string& qualified_name,
                                         const DescriptorPath& message_path,
                                         Diagnostic& error);
bool validate_message_map_entries(const SchemaFile& file,
                                  const DescriptorProto& message,
                                  const std::string& qualified_name,
                                  Diagnostic& error);

// 4. oneof layout and declarations: contiguous members (U-21), declarations
// claimed by no field (D-28), and the `proto3_optional` invariants (U-11).
// `is_proto3` is the file's own syntax: the proto3_optional flag means
// presence only there.
bool validate_message_oneofs(SchemaFile& file, const DescriptorProto& message,
                             const std::string& scope,
                             const DescriptorPath& message_path, bool is_proto3,
                             Diagnostic& error);

// 5. nested type names, then recursion
bool validate_nested_types(SchemaFile& file, const SymbolIndex& symbols,
                           const std::set<const SchemaFile*>& visible_files,
                           const UnresolvedPolicy& unresolved_policy, bool link_types,
                           DescriptorProto& message, const std::string& scope,
                           const DescriptorPath& message_path, Diagnostic& error);

} // namespace internal
} // namespace easypb_schema

#endif
