#ifndef EASYPB_SCHEMA_HPP_INCLUDED
#define EASYPB_SCHEMA_HPP_INCLUDED

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "descriptor.pb.hpp"

namespace easypb_schema {

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
    // silently downgraded to proto2.
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
    DIAGNOSTIC_MISSING_TYPE_NAME
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
