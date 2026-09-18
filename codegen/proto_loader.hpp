#ifndef EASYPB_PROTO_LOADER_HPP_INCLUDED
#define EASYPB_PROTO_LOADER_HPP_INCLUDED

#include <cstddef>
#include <string>
#include <vector>

#include "schema.hpp"

namespace easypb_proto {

struct SourceLoaderOptions {
    std::vector<std::string> proto_paths;
};

struct LoadStatistics {
    std::size_t files_read;
    std::size_t files_parsed;

    LoadStatistics() : files_read(0), files_parsed(0) {}
};

// Loads root schemas and their complete transitive dependencies exactly
// once, assigns stable logical names, and builds validated import edges
// without final type linking.
//
// Search roots are used in caller order; an empty list means the current
// working directory. The first matching root wins. Every file is read
// once and parsed with deferred type resolution; temporary source
// buffers are destroyed after each parse while pooled descriptor strings
// stay valid. Declaration order defines edges and CLI order defines
// targets. Weak imports have the same existence requirement as normal
// imports in this complete mode.
//
// Root arguments are accepted as logical names or existing disk paths,
// and a spelling naming an existing file is always treated as a disk
// path first, even when it would also be a valid logical name.
// Logical names are strict UTF-8. Windows physical paths supplied to this
// API are also UTF-8 and are converted to UTF-16 only at W-API calls. POSIX
// physical paths remain opaque native bytes; only a disk-derived suffix that
// becomes a logical name is required to pass the UTF-8 validator. Translating
// native CLI arguments into this API contract belongs to the later CLI step.
// An existing disk path is mapped to a logical name under a search root
// and verified against search precedence; a shadowed root is reported
// instead of compiling a different file, and an operational probe
// failure stops with a diagnostic instead of advancing the search.
// A disk path under a search root with an invalid remainder is
// rejected rather than treated as isolated. An isolated fully qualified root (drive-absolute or UNC on
// Windows, leading '/' elsewhere) outside all roots keeps its original
// name and must not contain imports; only lexically equivalent
// spellings of one isolated file deduplicate, different names of one
// file are a duplicate alias. A root outside all search roots that
// declares imports fails with an -I diagnostic even when the imports
// would otherwise resolve.
//
// The result starts empty and is cleared on failure. Statistics count
// successful reads/parses in the attempted session. No output files are
// written. Registering CLI flags and replacing the generation pipeline
// belong to a later plan; this loader is independently testable.
bool load_source_files(const SourceLoaderOptions& options,
                       const std::vector<std::string>& roots,
                       easypb_schema::SchemaSet& result,
                       easypb_schema::Diagnostic& error,
                       LoadStatistics* statistics = 0);

} // namespace easypb_proto

#endif
