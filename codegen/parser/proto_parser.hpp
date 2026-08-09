#ifndef EASYPB_PROTO_PARSER_HPP_INCLUDED
#define EASYPB_PROTO_PARSER_HPP_INCLUDED

#include <cstddef>
#include <string>
#include <vector>

#include "descriptor.pb.hpp"

namespace easypb_proto {

struct SourceLocation
{
    std::size_t offset;
    std::size_t line;
    std::size_t column;

    SourceLocation() : offset(0), line(1), column(1) {}
};

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

// Owns the descriptor tree and every string referenced by it. The source
// buffer passed to parse_proto() is borrowed only while parse_proto() runs.
class ParsedProto
{
public:
    StringPool strings;
    FileDescriptorProto file;
    std::vector<ImportInfo> imports;
    std::vector<Diagnostic> warnings;

    ParsedProto();
    void clear();

private:
    ParsedProto(const ParsedProto&);
    ParsedProto& operator=(const ParsedProto&);
};

bool parse_proto(const std::string& file_name,
                 const char* source,
                 std::size_t source_size,
                 ParsedProto& result,
                 Diagnostic& error);

// Convenience overload. parse_proto() copies every retained string into
// ParsedProto, so source may be destroyed immediately after this call returns.
inline bool parse_proto(const std::string& file_name,
                        const std::string& source,
                        ParsedProto& result,
                        Diagnostic& error)
{
    return parse_proto(file_name, source.data(), source.size(), result, error);
}

} // namespace easypb_proto

#endif
