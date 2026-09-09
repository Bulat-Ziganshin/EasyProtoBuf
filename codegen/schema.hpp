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
    DIAGNOSTIC_UNRESOLVED_TYPE
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
// Import edges are resolved by the source loader; until then edges is empty.
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

} // namespace easypb_schema

#endif
