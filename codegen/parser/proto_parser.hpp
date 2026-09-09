#ifndef EASYPB_PROTO_PARSER_HPP_INCLUDED
#define EASYPB_PROTO_PARSER_HPP_INCLUDED

#include <cstddef>
#include <string>
#include <vector>

#include "descriptor.pb.hpp"
#include "schema.hpp"

namespace easypb_proto {

// Storage, diagnostics, and schema ownership live in easypb_schema so the
// descriptor model and the semantic passes can be reused without the source
// parser. The names below remain visible here for compatibility.
typedef easypb_schema::SourceLocation SourceLocation;
typedef easypb_schema::DiagnosticCode DiagnosticCode;
using easypb_schema::DIAGNOSTIC_GENERIC;
using easypb_schema::DIAGNOSTIC_UNRESOLVED_TYPE;
typedef easypb_schema::Diagnostic Diagnostic;
typedef easypb_schema::ImportInfo ImportInfo;
typedef easypb_schema::StringPool StringPool;
typedef easypb_schema::SchemaFile ParsedProto;

struct ParseOptions {
    // When true, parse syntax, record declarations/imports/source positions,
    // and apply only checks that need no type resolution (duplicate
    // declarations, field numbers, oneof bounds, packed labels, scalar
    // packed/default rules), leaving every named field type unresolved
    // (has_type == false with the raw spelling kept in type_name).
    // Kind-dependent validation for named types (packed legality, enum
    // defaults) is left for import linking: a locally resolvable kind must
    // not decide the outcome, because shadowing imports may change the
    // resolved type. No unresolved warnings are produced. When false (the
    // default), keep the standalone behavior: resolve against local
    // declarations and warn about imports.
    bool defer_type_resolution;
    ParseOptions() : defer_type_resolution(false) {}
};

bool parse_proto(const std::string& file_name,
                 const char* source,
                 std::size_t source_size,
                 ParsedProto& result,
                 Diagnostic& error,
                 const ParseOptions& options);

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

inline bool parse_proto(const std::string& file_name,
                        const std::string& source,
                        ParsedProto& result,
                        Diagnostic& error,
                        const ParseOptions& options)
{
    return parse_proto(file_name, source.data(), source.size(),
                       result, error, options);
}

} // namespace easypb_proto

#endif
