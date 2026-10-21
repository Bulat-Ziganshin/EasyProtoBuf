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
// text goes. Every header is already generated, named and validated by the
// caller (`generate_files` maps the logical name through
// `generated_header_path`); this function adds the filesystem half, modelled
// on the reference compiler's WriteAllToDisk
// (compiler/command_line_interface.cc:541-610):
//
//   - two targets mapping to one output name are refused before the first
//     write (the reference compiler's own output map silently drops one);
//   - every output name is validated as a logical path, so a name that
//     escapes the output root is refused here as well as one layer up
//     (owner decision of 2026-10-21; the reference compiler writes such a
//     name one directory ABOVE the output root);
//   - the output directory must exist, and a file at that spelling is refused;
//   - the parent directories of each output are created, so a header nested
//     below the output root needs no directory made by hand;
//   - each file is created or truncated and written in place.
//
// Deliberately NOT provided, by owner decision of 2026-10-07, because the
// reference compiler does not provide it and the guards are measurably weaker
// than they look:
//
//   - containment below the output root. A name escaping the root is refused
//     by the name check above and, one layer up, by `generated_header_path`:
//     measured on a descriptor set naming "../escape.proto", ours refuses
//     (rc=1, nothing written) where protoc 30.1 writes one directory ABOVE
//     the output directory (rc=0). What is gone is the second, walk-based
//     half: a symlink below the output root is now followed, as it is in
//     protoc.
//   - letter-case collision and native identity. Measured on two targets
//     whose names differ only in case: before, rc=1
//     "one entry on this case-folding filesystem"; now rc=0 and one header,
//     the second target's content - identical to protoc 30.1, which also
//     writes one file. The removed check folded ASCII letters only, so a
//     non-ASCII pair already slipped through it (review finding CR-1 of
//     2026-10-05).
//   - atomic replacement. A process killed, or a disk that fills, mid-write
//     can now leave a truncated header, exactly as protoc can.
//
// Per-file replacement is therefore NOT guaranteed: a failure on a later file
// leaves earlier files fully written, and a failure during one write may
// leave that file partial. Cross-file atomicity is not promised at all, and
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
