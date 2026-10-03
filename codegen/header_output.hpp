#ifndef EASYPB_CODEGEN_HEADER_OUTPUT_HPP_INCLUDED
#define EASYPB_CODEGEN_HEADER_OUTPUT_HPP_INCLUDED

#include <string>
#include <vector>

#include "generation.hpp"
#include "schema.hpp"

namespace easypb_codegen {

// Publish generated headers below a physical output directory.
//
// The publisher is the only component that inspects or mutates an output
// tree (plan 5 P11): the generation core neither knows nor asks where its
// text goes. Every header is already generated and validated by the caller
// (`generate_files`); this function adds the filesystem half:
//
//   - every relative header name is validated again and mapped below the
//     output root, with lexical containment plus the native walk of
//     `confirm_fold_containment` (a link below the root is refused because
//     containment could not be proven);
//   - duplicate output names, letter-case aliases on a filesystem that folds
//     case (measured, never guessed), and hardlinks between two planned
//     outputs are refused before the first write;
//   - required subdirectories are created, and each file is replaced through
//     a temporary file in its own directory, so a failed replacement leaves
//     the old content, never a partial header.
//
// Per-file replacement is the guarantee. Cross-file atomicity is explicitly
// NOT promised: a failure on a later file leaves earlier files replaced, and
// no diagnostic or documentation claims otherwise.
//
// Returns false and fills `error` on the first failure; the result is not a
// partial-success report. `error.file` names the output directory or the
// offending output path.
bool publish_headers(const std::string& out_dir,
                     const std::vector<GeneratedFile>& files,
                     easypb_schema::Diagnostic& error);

} // namespace easypb_codegen

#endif
