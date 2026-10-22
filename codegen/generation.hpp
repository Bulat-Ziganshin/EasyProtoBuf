#ifndef EASYPB_CODEGEN_GENERATION_HPP_INCLUDED
#define EASYPB_CODEGEN_GENERATION_HPP_INCLUDED

#include <string>
#include <vector>

#include "generation_options.hpp"
#include "schema.hpp"
#include "schema_linker.hpp"

namespace easypb_codegen {

// One generated header: the relative logical name it is published under and
// its complete owning text. Neither field views descriptor storage, so the
// result outlives the schema if the caller wants it to.
struct GeneratedFile
{
    std::string name;
    std::string content;
};

// Generate one complete header from a linked target.
//
// The header carries the logical descriptor name as its provenance (the
// anonymous single-file `.pbs` compatibility path uses its container spelling
// instead, because that file has no logical name), the fixed runtime includes,
// the direct-dependency includes the include planner keeps, and the target's
// declarations and codecs. Imported definitions are made complete by their
// dependency headers, not by emitting them here; imported enums are named by
// initializers through the linked visible definitions, never inferred from
// zero or from a name.
//
// `schema.visible_files` decides which imported definitions are in scope.
// `schema.local_only` is not consulted here: the include planner emits every
// direct dependency in both link modes (W-68), and the compatibility
// tolerance decides which references the schema may keep, never whether the
// header names the file it came from.
//
// The result owns its text and keeps no view into the schema; common code
// never prints, never selects a stream and never exits. A semantic failure
// throws std::runtime_error (or std::exception from a helper); frontends that
// own a Diagnostic use generate_files, which converts the failure. The options
// are normalized by this call on a local copy (W-11), so a default-constructed
// set is valid input and the caller's object is not mutated.
std::string generate_header(const easypb_schema::SchemaFile& target,
                            const easypb_schema::LinkedSchema& schema,
                            const GenerationOptions& options);

// Generate every explicitly selected target of `schema` in requested order
// (SchemaSet::targets()), mapping each target's logical name through
// `generated_header_path`.
//
// All targets are generated and validated before the caller writes anything;
// on a semantic or generation failure `result` is empty and `error` is filled,
// with no partial success. Duplicate mapped output names (as strings) are
// refused here; platform-specific aliases (letter case on a folding
// filesystem, hardlinks) are the publisher's half. `result` follows the
// requested target order. The options are normalized on a local copy before
// the first target is generated (W-11), so a default-constructed set is valid
// and a contradictory pair is reported through `error` like any other
// semantic refusal.
bool generate_files(const easypb_schema::LinkedSchema& schema,
                    const GenerationOptions& options,
                    std::vector<GeneratedFile>& result,
                    easypb_schema::Diagnostic& error);

} // namespace easypb_codegen

#endif
