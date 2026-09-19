#ifndef EASYPB_PROTO_LOADER_HPP_INCLUDED
#define EASYPB_PROTO_LOADER_HPP_INCLUDED

#include <string>
#include <vector>

#include "schema.hpp"

namespace easypb_proto {

struct SourceLoaderOptions {
    std::vector<std::string> proto_paths;
};

// Loads root schemas and their complete transitive dependencies exactly
// once, assigns stable logical names, and builds validated import edges
// without final type linking.
//
// The contract follows the reference compiler (protoc) wherever the
// result is observable:
//   - search roots are used in caller order and the first root holding a
//     file wins; an empty list means the current working directory and an
//     empty entry means the same thing;
//   - an argument naming an existing file is a disk path: it is mapped to
//     the logical name it has under the first root that contains it, and
//     an input lying under no root is an error, exactly like protoc's
//     --proto_path requirement, and an input that names a directory is
//     reported as a directory rather than as a missing file. When the
//     mapped logical name resolves to
//     a different file under the ordered roots, the input is shadowed and
//     the load stops instead of compiling a different file;
//   - a spelling that names no existing file is a logical name resolved
//     through the ordered roots, while an empty argument is refused as
//     invalid input, which is also how protoc answers one;
//   - a root that cannot be reached (denied access) stops the search,
//     while absence alone advances to the next root, which is also where
//     protoc draws the line;
//   - every import must exist, weak imports included, and an import cycle
//     is an error; protoc reports both the same way;
//   - one file is parsed once per logical name, which is also the name
//     stored in the descriptor and used for generated include names.
//     Two different files under one logical name are not detected here
//     (protoc reports the duplicate definitions later instead), and two
//     logical names for one physical file simply load twice.
//
// Prefix comparison is byte-wise and case-sensitive on every platform,
// and the filesystem decides which spellings name which entry: a
// differently-spelled search root does not map, exactly like protoc's
// mapping. Windows physical paths supplied to this API are UTF-8 and are
// converted to UTF-16 only at W-API calls, as protoc's io_win32 layer
// does; POSIX physical paths remain opaque native bytes, and only a
// disk-derived suffix that becomes a logical name must pass the UTF-8
// validator. Translating native CLI arguments into this API contract
// belongs to the later CLI step.
//
// Deliberate differences from protoc are confined to diagnostics: this
// loader reports its own codes, source positions, the searched roots and
// the import chain, and it rejects import spellings that protoc would
// report as missing files (a NUL or other control byte, quotes, a
// malformed UTF-8 sequence, an absolute path). Arguments and roots are
// also normalized before mapping (see file_paths.hpp): ".." and Windows
// drive-relative spellings resolve here while protoc rejects them.
//
// Every file is read once and parsed with deferred type resolution;
// temporary source buffers are destroyed after each parse while pooled
// descriptor strings stay valid. Declaration order defines edges and CLI
// order defines targets. The result starts empty and is cleared on
// failure. No output files are written. Registering CLI flags and
// replacing the generation pipeline belong to a later step; this loader
// is independently testable.
bool load_source_files(const SourceLoaderOptions& options,
                       const std::vector<std::string>& roots,
                       easypb_schema::SchemaSet& result,
                       easypb_schema::Diagnostic& error);

} // namespace easypb_proto

#endif
