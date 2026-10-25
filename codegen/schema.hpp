#ifndef EASYPB_SCHEMA_HPP_INCLUDED
#define EASYPB_SCHEMA_HPP_INCLUDED

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "descriptor.pb.hpp"

namespace easypb_schema {

// The text of a descriptor view, as an owned string.
//
// An empty view may carry a null pointer - a default-constructed
// std::string_view does, and the descriptor adapter's intern_text returns
// exactly that for an empty descriptor string - and std::string(nullptr, 0)
// is formally undefined (the ledger's W-35). The empty case is spelled out
// here, in the one conversion door every schema frontend, the linker and the
// generator use; a local copy of this helper is the defect the door removes.
inline std::string view_text(const str_view& value)
{
    if (value.empty()) return std::string();
    return std::string(value.data(), value.size());
}

// A SourceLocation with line == 0 and column == 0 is an unknown location.
// It is used for descriptor-only data and for source metadata that has no
// meaningful token (for example a missing option). Real lexer locations
// always start at line 1, column 1.
struct SourceLocation
{
    std::size_t offset;
    std::size_t line;
    std::size_t column;

    SourceLocation() : offset(0), line(1), column(1) {}
};

inline SourceLocation unknown_location()
{
    SourceLocation location;
    location.offset = 0;
    location.line = 0;
    location.column = 0;
    return location;
}

enum DiagnosticCode
{
    DIAGNOSTIC_GENERIC,
    DIAGNOSTIC_UNRESOLVED_TYPE,
    DIAGNOSTIC_INVALID_IMPORT,
    DIAGNOSTIC_MISSING_IMPORT,
    DIAGNOSTIC_IMPORT_CYCLE,
    // Plan 4 (A3): a conflicting declaration, with both declarations named.
    // New values are appended, never inserted: an existing value must keep
    // its numeric identity for every caller that stores or compares codes.
    DIAGNOSTIC_DUPLICATE_SYMBOL,
    // Plan 4 Step 4.1 (A3, A9): the name exists in the compilation but its
    // defining file is not visible from the requesting file.
    DIAGNOSTIC_INVISIBLE_TYPE,
    // Plan 4 Step 4.1 (A3, A9): the name resolves to a declaration that is
    // not a type (a field, an enum value, a oneof, a service, or a package).
    DIAGNOSTIC_NOT_A_TYPE,
    // Plan 4 Step 4.2 (A3, A22/A24): a user field names a synthetic map-entry
    // message instead of the map field that owns it.
    DIAGNOSTIC_MAP_ENTRY_TYPE,
    // Plan 4 Step 5.1 (A3, A7): a descriptor-origin field's declared kind
    // disagrees with the definition its type_name resolves to. The declared
    // kind is authoritative metadata, so it is compared, never rewritten.
    DIAGNOSTIC_KIND_MISMATCH,
    // Plan 4 Step 5.1 (A26): a descriptor carries a synthetic map entry whose
    // shape is not the key/value pair protoc requires, referenced or not.
    DIAGNOSTIC_MALFORMED_MAP_ENTRY,
    // Plan 4 Step 5.1 (A25): a file's syntax value is neither proto2 nor
    // proto3 (protoc 30.1 writes "editions" for an editions file), so the
    // project cannot model its semantics. Refused loudly instead of being
    // silently downgraded to proto2. Both origins answer this code since
    // S7e (W-60): the descriptor path classifies in `link_schema`, and the
    // source parser throws it for an `edition` statement and for a `syntax`
    // value the classifier rejects.
    DIAGNOSTIC_UNSUPPORTED_SYNTAX,
    // Plan 4 Step 5.1 (contract clause 12, D-23): a descriptor-origin field
    // number outside 1..536870911 or inside the reserved 19000..19999 range.
    // The parser refuses both shapes as source text
    // (codegen/parser/proto_parser.cpp:1049-1060), so this is the
    // descriptor-origin half of that frontend parity rule.
    DIAGNOSTIC_INVALID_FIELD_NUMBER,
    // Plan 4 Step 5.1 (contract clause 12, D-23): a field whose declared kind
    // is a named kind (MESSAGE, ENUM, GROUP) but which carries no type_name,
    // so the generator cannot name the type it must emit. protoc's own
    // cross-link refuses the same shape (`Field with message or enum type
    // missing type_name.`, descriptor.cc's CrossLinkField).
    DIAGNOSTIC_MISSING_TYPE_NAME,
    // Plan 4 Step 5.1 (contract clause 12, D-28): two values of one enum share
    // a number and the enum does not declare `option allow_alias = true`.
    // protoc refuses the shape when it builds a descriptor ("... uses the same
    // enum value as ...") and our parser refuses it as source text
    // (codegen/parser/proto_parser.cpp:1769), which is why the option
    // travels in the model (`EnumOptions`) - without it the linker could not
    // tell an intentional alias from this defect. Both origins answer this
    // code since S7e (W-60).
    DIAGNOSTIC_DUPLICATE_ENUM_NUMBER,
    // Plan 4 Step 5.1 (contract clause 12, D-28): a message declares a oneof no
    // field points at, so the declaration is unreachable rather than a choice
    // set. protoc refuses the shape when it builds a descriptor (`Oneof must
    // have at least one field.`); the index-RANGE check beside this one only
    // refuses a field pointing outside the list, so a declaration nothing
    // points at is invisible to it.
    DIAGNOSTIC_EMPTY_ONEOF,
    // Plan 4 Step 5.1 (contract clause 12, D-32): an enum declares no value at
    // all, so the type has no member and no number to resolve. protoc refuses
    // the shape when it builds a descriptor (`Enums must contain at least one
    // value.`) and our parser refuses the source twin as text ("enum must
    // contain at least one value", proto_parser.cpp:1739), so both origins
    // answer this code since S7e (W-60). The check runs BEFORE the
    // `allow_alias` early return: an empty enum that writes the option is
    // still refused by protoc, so the option cannot licence an empty list.
    DIAGNOSTIC_EMPTY_ENUM,
    // Plan 4 Step 5.1 (contract clause 12, the 0927-mimo review's F2): a
    // descriptor-origin file that declares `syntax = "proto3"` carries a field
    // with the `required` label, which proto3 does not have. protoc refuses the
    // shape when it builds the file from the same bytes (`ValidateProto3Field`,
    // descriptor.cc:7917, "Required fields are not allowed in proto3.") and our
    // parser refuses its source twin (codegen/parser/proto_parser.cpp:1329),
    // so both origins answer this code since S7e (W-60). The gate is the
    // field's OWN file: proto2 gives the label a meaning, so reading the
    // wrong file here would refuse legal input.
    DIAGNOSTIC_PROTO3_REQUIRED_FIELD,
    // Plan 4 Step 5.1 (contract clause 12, the 0927-mimo review's F2): the same
    // gate for a proto3 field that carries an explicit `default_value`, which
    // proto3 does not allow. protoc refuses it at the same site
    // (descriptor.cc:7922, "Explicit default values are not allowed in
    // proto3.") and the parser refuses the source twin
    // (codegen/parser/proto_parser.cpp:1192), so the two codes are one rule
    // asked of two shapes: the shape, not the file class, is what the message
    // names. Both origins answer the code since S7e (W-60).
    DIAGNOSTIC_PROTO3_EXPLICIT_DEFAULT,
    // Plan 4 clause 13 (ledger `D-39`, the 0927-mimo review's F9): a declaration
    // name the descriptor carries is not a legal protobuf symbol name - empty,
    // or carrying a character outside `[A-Za-z0-9_]`. protoc refuses both while
    // it builds the file (`ValidateSymbolName`, descriptor.cc:5419-5438:
    // "Missing name." for the empty one, `"<name>" is not a valid identifier.`
    // otherwise) and applies the rule to every declaration and to every package
    // component; the SOURCE path cannot produce either shape (the tokenizer
    // only yields identifier tokens), so this is the descriptor-origin half of
    // the same frontend parity `D-23`/`D-28`/`D-32`/`D-36` carry. protoc imposes
    // NO first-character rule (a name starting with a digit is accepted there),
    // so this code must not add one: whether a name is usable as a C++ name is
    // the generator's question, not the linker's (contract clause 11). The two
    // messages share one code because they are one category - the name is not a
    // symbol name - and one rule asked of two values (E1/A3).
    DIAGNOSTIC_INVALID_SYMBOL_NAME,
    // Plan 5 follow-up S1 (W-10): a descriptor-origin field declares a kind
    // the generator cannot emit - no kind at all, a group, or a value outside
    // the TYPE_* set. protoc resolves all three (an absent kind as the proto
    // default TYPE_DOUBLE, a group as a generated group class) and emits
    // working code; this project REFUSES them, because the emitter has no
    // spelling for them and used to write "???type" / "???group" into the
    // header. A deliberate "stricter than protoc" divergence, like the
    // package-depth and the output-root rules. The check is gated on the
    // descriptor origin, because a source-origin field carries no declared
    // kind: deferred parsing leaves has_type unset and the kind is written
    // during resolution.
    DIAGNOSTIC_INVALID_FIELD_TYPE,
    // Plan 5 follow-up S5 (W-44): the first value of a proto3 enum is not
    // zero. protoc refuses the shape when it builds the file ("The first enum
    // value must be zero for open enums.", descriptor.cc:8245) and our parser
    // refuses the source twin as text ("the first proto3 enum value must be
    // zero", proto_parser.cpp's enum rule), so this is the descriptor-origin
    // half of that frontend parity rule. Only a proto3 file is asked: proto2
    // gives the first value no special meaning. The check rides the
    // reference-free enum sweep, so an enum nothing references is covered too.
    DIAGNOSTIC_PROTO3_ENUM_FIRST_VALUE,
    // Plan 5 follow-up S5 (W-45): an extension range a descriptor declares is
    // not a range protoc would build. Four shapes share the code because they
    // are one category - the declaration cannot describe extension numbers:
    // a range in a proto3 file ("Extension ranges are not allowed in
    // proto3.", descriptor.cc:7898), a non-positive start, a range whose
    // exclusive end does not exceed its start (`BuildExtensionRange`,
    // descriptor.cc:6884-6898), a range that includes a declared field number
    // ("Extension range $0 to $1 includes field \"$2\" ($3).",
    // descriptor.cc:6426-6430), and two overlapping ranges (`:6454-6482`).
    // The source path refuses every one of them as text
    // (proto_parser.cpp:1761/1794/1814), so this is the descriptor-origin half
    // of the same parity rule. Descriptor-origin only: the parser refuses the
    // same shapes as text before they reach the model, and its ranges are not
    // retained in the descriptor tree at all.
    DIAGNOSTIC_INVALID_EXTENSION_RANGE,
    // Step B5 (root R3, CR-06): a message or enum `reserved` declaration a
    // descriptor carries is not one protoc would build. Every shape shares one
    // code because they are one category - the declaration cannot describe
    // numbers or names a declaration is forbidden to use: a message range whose
    // start is not positive or whose exclusive end does not exceed its start
    // (`BuildReservedRange`, descriptor.cc:6906-6922), an enum range whose
    // inclusive end precedes its start (`:6924-6935`), two overlapping ranges
    // (`:6396-6410`, `:7076-7092`), a reserved name written twice, a field or
    // enum value that uses a reserved number or name (`:6412-6480`,
    // `:7094-7125`), and an extension range that overlaps a reserved range
    // (`:6447-6452`). protoc refuses all of them while it builds a file; the
    // source parser refuses the same shapes as text (`proto_parser.cpp`'s
    // `parse_reserved`, `add_range`, `validate_message_constraints` and the
    // enum value checks), so this is the descriptor-origin half of the same
    // frontend parity rule. Descriptor-origin only: the parser does not retain
    // its ranges in the descriptor tree.
    DIAGNOSTIC_INVALID_RESERVED_RANGE,
    // Step B5 (root R3, CR-23): a message declares
    // `MessageOptions.message_set_wire_format = true` and also declares
    // ordinary fields. protoc refuses the shape ("MessageSets cannot have
    // fields, only extensions.", descriptor.cc:6372-6380, :7994-8001), because
    // the wire encoding of a MessageSet is not an ordinary message. The source
    // parser does not model message options at all (B1 gap G2), so this is the
    // descriptor-origin half of the rule; the field-less MessageSet protoc
    // accepts is accepted here too.
    DIAGNOSTIC_MESSAGE_SET_WITH_FIELDS,
    // Step B8 (root R6, CR-22): a CUSTOM `json_name` is shaped like the
    // extension spelling - it starts with '[' and ends with ']'. protoc refuses
    // it while it builds the file ("The custom JSON name of field \"a\"
    // (\"[foo]\") is invalid: JSON names may not start with '[' and end with
    // ']'.", descriptor.cc's `CheckFieldJsonNameUniqueness`, :6519-6531,
    // through `JsonNameLooksLikeExtension`) because the bracket spelling is
    // reserved for extensions. The rule is a NAME rule, not a
    // duplicate-symbol one, and it applies to both syntaxes. Both frontends
    // answer it: the parser writes the option into the descriptor, and the
    // check rides the reference-free `validate_json_names` sweep.
    DIAGNOSTIC_INVALID_JSON_NAME
};

struct Diagnostic
{
    std::string file;
    SourceLocation location;
    std::string message;
    bool warning;
    DiagnosticCode code;

    Diagnostic() : warning(false), code(DIAGNOSTIC_GENERIC) {}
};

// Plan 4 amendment A25: one classification of a file's syntax, shared by every
// component that must not guess. Absent and "proto2" are proto2; "proto3" is
// proto3; anything else - protoc 30.1 writes "editions" for a file declared
// with `edition = "2023";` - is an unsupported input, because enum openness and
// field presence come from the edition's features, which this model does not
// carry. Declared here and defined in schema.cpp so the parser-free profile can
// refuse it too; link_schema refuses FILE_SYNTAX_UNSUPPORTED in both modes
// before any resolution work.
enum FileSyntax
{
    FILE_SYNTAX_PROTO2,        // syntax absent, or exactly "proto2"
    FILE_SYNTAX_PROTO3,        // syntax exactly "proto3"
    FILE_SYNTAX_UNSUPPORTED    // any other value, including "editions"
};

// descriptor.proto's Edition enum values the classifier can read as a file
// class (`FileDescriptorProto.edition`, field 14). protoc's `BuildFileImpl`
// consults `has_edition()` BEFORE the `syntax` string (descriptor.cc:5952-
// 5971), so a crafted set carrying the field with no syntax string is
// classified like the reference instead of being silently downgraded to
// proto2 (ledger `D-52`). Only these two editions are modeled; EDITION_2023
// and every other value are an unsupported input, exactly as the string
// "editions" is (A25). The two editions protobuf 30.1 can write beyond them
// are named here so A25's refusal can say which edition it read instead of
// quoting an absent `syntax` string (ledger `D-61`).
enum
{
    kEditionProto2 = 998,
    kEditionProto3 = 999,
    kEdition2023 = 1000,
    kEdition2024 = 1001
};

FileSyntax classify_file_syntax(const FileDescriptorProto& file);

struct ImportInfo
{
    enum Modifier {
        NORMAL_IMPORT,
        PUBLIC_IMPORT,
        WEAK_IMPORT
    };

    std::string path;
    Modifier modifier;
    SourceLocation location;

    ImportInfo() : modifier(NORMAL_IMPORT) {}
};

class StringPool
{
public:
    StringPool();
    ~StringPool();

    str_view save(const std::string& value);
    str_view save(const char* data, std::size_t size);
    void clear();

private:
    std::vector<char*> blocks_;
    std::vector<std::size_t> capacities_;
    std::vector<std::size_t> used_;

    StringPool(const StringPool&);
    StringPool& operator=(const StringPool&);
};

// A descriptor path follows the official protobuf convention: an alternating
// sequence of field numbers and indexes into repeated fields, starting from
// a FileDescriptorProto. For example [4, 0, 2, 1] addresses the second field
// (field 2 of DescriptorProto) of the first top-level message (field 4 of
// FileDescriptorProto).
typedef std::vector<int> DescriptorPath;

// The deepest message nesting a schema may declare. The reference compiler
// has the same limit and draws the line in the same place: its
// MaxMessageDeclarationNestingDepth() is 32 (descriptor.h:3045) and it
// refuses the 32nd level of nested messages - 31 levels are accepted, the
// 32nd is not (compiler/parser.cc:785-789 and descriptor.cc:6364-6373, both
// with the sentence "Reached maximum recursion limit for nested messages.").
//
// The rule lives here because three doors enforce it and they must not
// disagree: the source parser refuses to descend deeper, the descriptor
// loader refuses to decode a set that nests deeper, and the semantics layer
// refuses a schema a library caller built by hand. A schema that passes all
// three cannot make any later walk of the nesting recurse without a bound.
//
// The depth of a message is the number of LEVELS of its descriptor path: the
// path holds a field number and an index per level
// (easypb_schema::file_message_path, message_nested_path), so a file-level
// message has two components and a message nested in it has four.
inline int max_message_nesting_depth()
{
    return 32;
}

class SchemaFile;

struct FieldSource
{
    DescriptorPath path;
    std::string raw_type_name;
    SourceLocation type_location;
    SourceLocation default_location;
    SourceLocation packed_location;
};

struct ImportEdge
{
    SchemaFile* target;
    std::size_t dependency_index;
    ImportInfo::Modifier modifier;
};

// Owns one file's descriptor tree and every string referenced by it. The
// file descriptor and all retained strings belong to the member pool.
// Source files additionally carry per-field provenance (field_sources),
// declaration positions (declaration_locations), and the location-bearing
// import list (imports). The descriptor dependency lists in file
// (dependency, public_dependency, weak_dependency) are authoritative for
// graph meaning; imports supplies positions, not a second import policy.
// Import edges are built by bind_import_edges for every frontend; until
// then edges is empty.
class SchemaFile
{
public:
    StringPool strings;
    FileDescriptorProto file;
    std::vector<ImportInfo> imports;
    std::vector<Diagnostic> warnings;
    std::vector<FieldSource> field_sources;
    std::map<DescriptorPath, SourceLocation> declaration_locations;
    std::vector<ImportEdge> edges;
    std::string physical_name;
    bool from_descriptor_set;

    SchemaFile();
    void clear();

private:
    SchemaFile(const SchemaFile&);
    SchemaFile& operator=(const SchemaFile&);
};

// The label a diagnostic uses for a file: the logical name when the file has
// one, the container path it came from otherwise (the anonymous single-file
// `.pbs` shape, where the name is cleared on purpose). A failure must never
// render an empty label for a file the user named on the command line (W-19).
std::string schema_file_label(const SchemaFile& file);

// Owns a set of schema files. Files live on the heap, so their addresses
// stay stable while the owning vector grows. Logical-name index keys are
// owned strings. Duplicate logical names are rejected with an exception.
// Callers own the one-logical-name-one-file invariant: check find_file (or a
// discovery cache) before add_file and report name clashes through
// Diagnostic, so loader/adapter bool APIs never leak this exception.
// Targets are deduplicated in first-requested order.
//
// A caller must not clear one member of a linked schema set while other
// objects still reference it.
class SchemaSet
{
public:
    SchemaSet();
    ~SchemaSet();

    SchemaFile& add_file(const std::string& logical_name);
    // Adds a file with no logical name: the anonymous single-file `.pbs`
    // shape. `identity` is the container path the file came from; it becomes
    // physical_name only and is deliberately NOT inserted into the name index
    // (`--target <container>` must not select a file the descriptor does not
    // name, W-18). The descriptor keeps an empty, absent name view.
    SchemaFile& add_unnamed_file(const std::string& identity);
    SchemaFile* find_file(const std::string& logical_name) const;
    const std::vector<std::unique_ptr<SchemaFile> >& files() const;
    void add_target(SchemaFile& file);
    const std::vector<SchemaFile*>& targets() const;
    void clear();

private:
    std::vector<std::unique_ptr<SchemaFile> > files_;
    std::map<std::string, SchemaFile*> index_;
    std::vector<SchemaFile*> targets_;

    SchemaSet(const SchemaSet&);
    SchemaSet& operator=(const SchemaSet&);
};

// Descriptor path constructors. Indexes are declaration indexes within the
// containing repeated field.
inline DescriptorPath file_message_path(int message_index)
{
    DescriptorPath path;
    path.push_back(4);
    path.push_back(message_index);
    return path;
}

inline DescriptorPath file_enum_path(int enum_index)
{
    DescriptorPath path;
    path.push_back(5);
    path.push_back(enum_index);
    return path;
}

inline DescriptorPath file_service_path(int service_index)
{
    DescriptorPath path;
    path.push_back(6);
    path.push_back(service_index);
    return path;
}

// `ServiceDescriptorProto.method` is field 2, and a method's name is field 1 of
// that message (descriptor.proto). Used by the lexical name sweep, which covers
// method declarations too (ledger `D-57`).
inline DescriptorPath service_method_path(const DescriptorPath& service_path,
                                          int method_index)
{
    DescriptorPath path = service_path;
    path.push_back(2);
    path.push_back(method_index);
    return path;
}

// `MethodDescriptorProto.input_type` is field 2 and `.output_type` is field 3
// (descriptor.proto). Unlike the name, the two are references, so a diagnostic
// about one names the type field itself; `field_number` is 2 or 3.
inline DescriptorPath service_method_type_path(const DescriptorPath& method_path,
                                               int field_number)
{
    DescriptorPath path = method_path;
    path.push_back(field_number);
    return path;
}

inline DescriptorPath message_field_path(const DescriptorPath& message_path,
                                         int field_index)
{
    DescriptorPath path = message_path;
    path.push_back(2);
    path.push_back(field_index);
    return path;
}

inline DescriptorPath message_nested_path(const DescriptorPath& message_path,
                                          int nested_index)
{
    DescriptorPath path = message_path;
    path.push_back(3);
    path.push_back(nested_index);
    return path;
}

inline DescriptorPath message_enum_path(const DescriptorPath& message_path,
                                        int enum_index)
{
    DescriptorPath path = message_path;
    path.push_back(4);
    path.push_back(enum_index);
    return path;
}

inline DescriptorPath message_oneof_path(const DescriptorPath& message_path,
                                         int oneof_index)
{
    DescriptorPath path = message_path;
    path.push_back(8);
    path.push_back(oneof_index);
    return path;
}

// `DescriptorProto.extension_range` is field 5. The model carries the ranges
// themselves since `W-45`, so a range can be named by a descriptor path like
// every other declaration.
inline DescriptorPath message_extension_range_path(
    const DescriptorPath& message_path, int range_index)
{
    DescriptorPath path = message_path;
    path.push_back(5);
    path.push_back(range_index);
    return path;
}

// `DescriptorProto.reserved_range` is field 9 and `DescriptorProto.reserved_name`
// is field 10. Both are modeled since step B5 (CR-06), so a reserved
// declaration can be named by a descriptor path like every other one.
inline DescriptorPath message_reserved_range_path(
    const DescriptorPath& message_path, int range_index)
{
    DescriptorPath path = message_path;
    path.push_back(9);
    path.push_back(range_index);
    return path;
}

inline DescriptorPath message_reserved_name_path(
    const DescriptorPath& message_path, int name_index)
{
    DescriptorPath path = message_path;
    path.push_back(10);
    path.push_back(name_index);
    return path;
}

// `DescriptorProto.options` is field 7. A rule about a message option (the
// MessageSet field rule, CR-23) points at the option record rather than at the
// message body: the option is what the declaration contradicts.
inline DescriptorPath message_options_path(const DescriptorPath& message_path)
{
    DescriptorPath path = message_path;
    path.push_back(7);
    return path;
}

// `EnumDescriptorProto.reserved_range` is field 4 and its `reserved_name` is
// field 5. The enum twin of the two message helpers above (CR-06).
inline DescriptorPath enum_reserved_range_path(
    const DescriptorPath& enum_path, int range_index)
{
    DescriptorPath path = enum_path;
    path.push_back(4);
    path.push_back(range_index);
    return path;
}

inline DescriptorPath enum_reserved_name_path(const DescriptorPath& enum_path,
                                              int name_index)
{
    DescriptorPath path = enum_path;
    path.push_back(5);
    path.push_back(name_index);
    return path;
}

inline DescriptorPath enum_value_path(const DescriptorPath& enum_path,
                                      int value_index)
{
    DescriptorPath path = enum_path;
    path.push_back(2);
    path.push_back(value_index);
    return path;
}

// Path lookups. Every function validates the path before dereferencing it
// and returns null when the path does not address an existing element.
const DescriptorProto* find_message(const SchemaFile& file,
                                    const DescriptorPath& path);
const EnumDescriptorProto* find_enum(const SchemaFile& file,
                                     const DescriptorPath& path);
const FieldDescriptorProto* find_field(const SchemaFile& file,
                                       const DescriptorPath& path);
const FieldSource* find_field_source(const SchemaFile& file,
                                     const DescriptorPath& path);

// protoc's own rule for naming the nested entry message a `map` field implies:
// an underscore capitalizes the next character only when it is a lowercase
// ASCII letter, every other character is copied through and clears the flag,
// and the result carries the "Entry" suffix. There is deliberately no
// empty-name fallback - `map<string, int32> _ = 1;` yields the entry `Entry`
// in protoc, and any other answer is a divergence from the reference
// compiler. Measured with libprotoc 30.1 against the naming table in
// tests/codegen/schema/test_semantics.cpp (plan-4 amendments A14, A15).
//
// It lives here rather than in the parser because the linker also builds in
// the descriptor-only profile, which links no parser, and because acceptance
// must not depend on which frontend produced a descriptor (amendment A24).
std::string map_entry_name(const std::string& field_name);

// Rebuilds every file's import edges from its authoritative descriptor
// dependency lists (dependency, public_dependency, weak_dependency) in
// dependency-list order. Source coordinates come from imports only and
// are never used for graph meaning, so this also works for
// descriptor-origin files with empty physical_name and no parser linked.
//
// Validation covers dependency spellings, dependency indexes/modifiers
// (negative, out-of-range, duplicate, overlapping public/weak) and
// repeated dependency names. Missing targets are errors when
// allow_missing is false (complete source loading and complete linking);
// when allow_missing is true a missing target yields an edge with a null
// pointer that stays identifiable by its dependency index (the explicit
// single-file descriptor compatibility path in a later plan). Cycles are
// always rejected, including cycles among available files when some
// edges are missing.
//
// The binder never opens files and never falls back to the source
// loader: names resolve only inside the supplied SchemaSet. A missing
// entry in a plugin-style closure is a request/graph error, not a
// search-path problem.
//
// On success every edge is bound and the function returns true. On
// failure it clears all edges, fills error (failed import, importing
// logical file, source position where available, cycle chain) and
// returns false.
bool bind_import_edges(SchemaSet& files, bool allow_missing,
                       Diagnostic& error);

} // namespace easypb_schema

#endif
