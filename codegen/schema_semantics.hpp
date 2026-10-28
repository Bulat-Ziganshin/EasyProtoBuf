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

// The `packed` value a field has before the command-line overrides: the
// field's own option when it is written, and otherwise the schema default -
// a repeated packable field of a proto3 file is packed, every other field is
// not. The emitter and the descriptor merge must answer this question the
// same way, or two containers holding one logical file could disagree about
// the wire format they generate (U-04). `syntax` is the classification of the
// field's own file (`classify_file_syntax`), never a second reading of the
// syntax string.
bool effective_packed(const FieldDescriptorProto& field, FileSyntax syntax);

bool valid_map_key_type(int type);

// True when `text` is an IDENTIFIER in the reference compiler's sense:
// `[A-Za-z_][A-Za-z0-9_]*`, ASCII only (`io::Tokenizer::IsIdentifier`). The
// source parser asks it for the `reserved "..."` warning (U-50) and the
// semantic pass asks it for enum defaults, so both fronts answer with one rule.
bool is_proto_identifier(const std::string& text);

bool parse_unsigned_integer(const std::string& text, std::uint64_t& result);
bool parse_signed_32(const std::string& text, std::int32_t& result);

// The adapter's gate for every canonicalizable default family: a text may be
// normalized unless it starts with '+' (the W-40 refusal) or ASCII whitespace
// (the W-03 refusal). Keeping the refusal spellings out of the normalizer is
// what lets the validator report them instead of hiding them behind a
// rewritten value (CR-02).
bool can_normalize_default_text(const std::string& text);

// Canonicalize a floating-point default the way protobuf's SimpleFtoa /
// SimpleDtoa do. `single_precision` narrows the parsed value to float before
// formatting; a double keeps its own precision. The function accepts the same
// spellings the source grammar and the descriptor field carry, including
// `inf`/`infinity` and `nan`/`nan(payload)` in any ASCII case (CR-02). An
// out-of-range parse is not a refusal: overflow becomes `inf`/`-inf`,
// underflow to zero becomes `0`/`-0`, and a finite subnormal becomes the
// shortest decimal that round-trips, exactly as the reference compiler stores
// the value (W-04). A text with no digits, trailing text, or a malformed
// spelling returns false and leaves `result` untouched.
bool canonical_float_default(const std::string& text, bool single_precision,
                             std::string& result);

// The value overload, for the source parser's INTEGER-token branch: format an
// already-parsed double exactly as the text overload would. It always
// succeeds.
bool canonical_float_default(double value, bool single_precision,
                             std::string& result);

// True when `text` is exactly the text `canonical_float_default` writes for
// its value. The adapter stores that form; the default validator and the
// emitter accept nothing else, so a spelling cannot be re-interpreted at a
// later layer (CR-02, R1).
bool is_canonical_float_default(const std::string& text,
                                bool single_precision);

// Parse a source INTEGER token or a descriptor integer default: an optional
// '-' followed by a decimal, octal (leading 0) or hex (leading 0x/0X)
// magnitude. `negative` reports the sign and `magnitude` the absolute value.
// Returns false for an empty or lone-sign text, a digit outside the base, or
// a magnitude above uint64.
bool parse_integral_default_literal(const std::string& text, bool& negative,
                                    std::uint64_t& magnitude);

// The canonical decimal spelling of an integral default. protoc stores every
// integral default as the decimal value it parsed (`parser.cc:1265-1307`,
// `descriptor.cc:2835`), so `010`, `0x10` and `-0` become `8`, `16` and `0`
// on both input paths (CR-17). Returns false and leaves `result` untouched
// when `parse_integral_default_literal` rejects the text.
bool canonical_integral_default(const std::string& text, std::string& result);

// Convert a source INTEGER token to the value a float/double default carries.
// This is protoc's `ConsumeNumber` integer branch (`parser.cc:231-255`): the
// token is parsed with the octal/hex-aware integer parser, and a decimal
// token above uint64 falls back to a floating parse. An octal/hex token that
// is malformed or out of range returns false (protoc's "Integer out of
// range."). `text` may carry a leading '-'.
bool integer_token_as_double(const std::string& text, double& result);

// The default JSON spelling of a protobuf field name: every `_` is dropped and
// the following ASCII letter is uppercased (`descriptor.cc:123-140`). The same
// helper feeds the source frontend's `json_name` option, the pretty printer,
// and the uniqueness validator so they cannot disagree.
std::string to_json_name(const std::string& name);

// Decode the C-escaped text a bytes default carries into the bytes it
// denotes. The grammar is the one `valid_bytes_default_escape` accepts and
// the one the reference compiler applies with `absl::CUnescape` while it
// builds a bytes field (descriptor.cc:6728-6741): the simple escapes, an
// octal escape of one to three digits, a hex escape of one or more digits
// and `\u`/`\U` of exactly four/eight hex digits, the last two decoding to
// the UTF-8 encoding of the code point. A string default is stored DECODED
// and needs no decoding - only a bytes default does.
//
// Returns false and leaves `result` untouched when the text is not a valid
// escape sequence. The generator core links this module, so the emitter can
// decode a descriptor-origin bytes default without depending on the parser.
bool decode_bytes_escape(const std::string& text, std::string& result);

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

// Validates every enum the file declares, top-level and nested, over the
// available descriptor alone: an enum must declare at least one value, reported
// as DIAGNOSTIC_EMPTY_ENUM (fix round r16, ledger D-32), and two values may
// share a number only when the enum declares `option allow_alias = true`,
// reported as DIAGNOSTIC_DUPLICATE_ENUM_NUMBER. The presence question is asked
// first, so an empty enum that writes the option is still refused - protoc
// refuses it. An enum nothing references is still
// descriptor input, so the check is unconditional, reference-free and runs
// before any resolution, beside the entry-shape sweep (A26's precedent) - the
// top-level enum is the entity no resolution pass walks (fix round r13, ledger
// D-28, contract clause 12). The source path refuses the same shape as text
// (`codegen/parser/proto_parser.cpp`'s duplicate-number rule) and records the
// option in the model, so this is the descriptor-origin half of that frontend
// parity rule. `link_schema` runs it in complete mode and the compatibility
// pass repeats it before its own finish pass, because neither mode consults a
// reference for the answer. Returns false and fills error at the first empty
// enum or the first repeated number, in declaration order.
bool validate_enum_value_numbers(SchemaFile& file, Diagnostic& error);

// Validates the JSON-name uniqueness protoc enforces per message (W-43).
// Two default names that fold to one JSON spelling are an error in proto3 and
// a warning in proto2, matching the reference compiler's LEGACY_BEST_EFFORT
// behaviour. Two custom `json_name` values that collide are an error in both
// syntaxes; a custom name that collides with a default one follows the same
// proto2-warning/proto3-error split. A pair of two non-custom names is
// reported by the first pass only. A CUSTOM name shaped like the extension
// spelling (`[...]`) is refused with DIAGNOSTIC_INVALID_JSON_NAME in both
// syntaxes, exactly as protoc's `JsonNameLooksLikeExtension` does (CR-22).
// Runs in both linking modes on the descriptor alone, before resolution and
// indexing, like the enum-number sweep beside it; a proto2 warning is stored
// on the file and printed after a successful link.
bool validate_json_names(SchemaFile& file, Diagnostic& error);

// Validates every extension range the file's messages declare, top-level and
// nested, over the available descriptor alone (plan 5 follow-up S5, W-45).
// The shapes share DIAGNOSTIC_INVALID_EXTENSION_RANGE, because they are one
// category - the declaration does not describe usable extension numbers: a
// range in a proto3 file, a non-positive start, an exclusive end that does not
// exceed its start, an exclusive end above 536870912 (the protocol maximum
// plus one, CR-07, unless the message declares MessageSet wire format), a
// range that includes a declared field number, and two overlapping ranges.
// Step R04 (U-22) adds the range's options: an explicit `verification =
// UNVERIFIED` beside any declaration, a declaration number outside its range,
// the same number declared twice, and a declaration carrying only one of
// `full_name`/`type` without being `reserved`. protoc refuses all ten while it
// builds a file (descriptor.cc:6884-6896, :7898, :6426-6430, :6454-6482,
// :8395-8404, :8377-8425) and the source path refuses the range shapes as text
// (proto_parser.cpp's extension-range parser), so this is the descriptor-origin
// half of the same frontend parity rule. An unreferenced message is still
// descriptor input, so the check is unconditional and runs before any
// resolution, in both linking modes, beside the entry-shape and enum-number
// sweeps (A26's precedent). Returns false and fills error at the first
// offending range, in declaration order.
bool validate_extension_ranges(SchemaFile& file, Diagnostic& error);

// Validates every message and enum `reserved` declaration the file carries,
// top-level and nested, over the available descriptor alone (step B5, CR-06).
// The shapes share DIAGNOSTIC_INVALID_RESERVED_RANGE, because they are one
// category - the declaration cannot describe numbers or names a declaration is
// forbidden to use: a message range whose start is not positive or whose
// exclusive end does not exceed its start, an enum range whose inclusive end
// precedes its start, two overlapping ranges, a reserved name written twice, a
// field or enum value that uses a reserved number or name, and an extension
// range that overlaps a reserved range. protoc refuses all of them while it
// builds a file (descriptor.cc:6396-6482, :6906-6935, :7060-7125); the source
// path refuses the same shapes as text (proto_parser.cpp's reserved parsing
// and value checks), so this is the descriptor-origin half of the same frontend
// parity rule. Reference-free and unconditional: an unreferenced message or
// enum is still descriptor input, so it runs in both linking modes beside the
// extension-range and enum-number sweeps. Returns false and fills error at the
// first offending declaration, in declaration order.
bool validate_reserved_declarations(SchemaFile& file, Diagnostic& error);

// Validates that no message declaring MessageSet wire format also declares
// ordinary fields (step B5, CR-23), top-level and nested. protoc refuses the
// shape while it builds a field ("MessageSets cannot have fields, only
// extensions.", descriptor.cc:6372-6380, :7994-8001) because the wire encoding
// of the message is not the ordinary one; a field-less MessageSet, and a
// MessageSet whose only members are extensions, are accepted exactly as protoc
// accepts them. Reference-free and unconditional; runs in both linking modes
// beside the reserved and extension-range sweeps. Returns false and fills error
// at the first offending message, in declaration order.
bool validate_message_set_wire_format(SchemaFile& file, Diagnostic& error);

// Validates the message nesting of one descriptor: no message may be nested
// deeper than easypb_schema::max_message_nesting_depth() levels, which is
// the reference compiler's own limit (MaxMessageDeclarationNestingDepth(),
// descriptor.h:3045). The reference refuses the 32nd level of nested messages
// with "Reached maximum recursion limit for nested messages."
// (compiler/parser.cc:785-789, descriptor.cc:6364-6373), and this carries
// the same sentence.
//
// The check exists because three walks follow the nesting and none of them
// may run unbounded: the source parser descends into the nested messages,
// the runtime decoder decodes them, and the linker walks the linked schema.
// The rule is enforced at each of those doors, and this function is the one
// the descriptor door and the library door ask.
//
// The walk behind it uses an explicit stack and stops at the limit, so it
// cannot overflow the stack on the very input it refuses. `label` names the
// file in the diagnostic; the offending message is named by its descriptor
// path.
bool validate_message_nesting_depth(const FileDescriptorProto& file,
                                    const std::string& label,
                                    Diagnostic& error);

// Validates every declaration name in the file lexically, over the available
// descriptor alone (contract clause 13; fix round r18, ledger D-39): a name must
// be non-empty and carry only `[A-Za-z0-9_]`, which is protoc's own
// `ValidateSymbolName` rule (`descriptor.cc:5419-5438`). It applies to every
// modeled declaration - message (top-level and nested), field, enum, enum value,
// oneof and service - and to every package component; the file's own `name` is a
// logical path and is NOT a symbol, so it is not checked. Both shapes carry
// DIAGNOSTIC_INVALID_SYMBOL_NAME, because they are one category. The check is
// reference-free and runs before any resolution, in both linking modes, beside
// the entry-shape and enum-number sweeps (A26's precedent): a malformed name is
// provable from the descriptor alone and an unreferenced declaration still
// reaches the generator. The source path cannot produce either shape (its
// tokenizer only yields identifier tokens), so this is the descriptor-origin
// half of the same frontend parity D-23/D-28/D-32/D-36 carry. protoc imposes no
// first-character rule, so neither does this. Returns false and fills error at
// the first malformed name, in declaration order.
bool validate_symbol_names(SchemaFile& file, Diagnostic& error);

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
