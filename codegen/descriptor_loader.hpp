#ifndef EASYPB_DESCRIPTOR_LOADER_HPP_INCLUDED
#define EASYPB_DESCRIPTOR_LOADER_HPP_INCLUDED

#include <string>
#include <vector>

#include "schema.hpp"

namespace easypb_schema {

// Standalone `.pbs` file reading and decoding: the filesystem half of
// descriptor ingestion. It lives outside the common library because the
// common library never touches the disk. Each container is read and decoded
// while its bytes are alive, then handed to append_descriptor_files, which
// takes the owned copy; the container may be deleted immediately after this
// call, and no common operation requires its path to exist.
//
// The standalone wrapper may retain each container path in physical_name;
// nothing reads that field as a disk path. Descriptor-origin files carry
// `from_descriptor_set = true` and no invented source coordinates.
//
// Legacy input policy: a container holding exactly one file whose logical
// name is absent keeps the anonymous single-file `.pbs` stdout compatibility
// path alive. Such a file is addressed by an internal identity derived from
// the container path, which keys the set and labels diagnostics; the
// identity is never written into the descriptor - the copied file keeps an
// empty `name` and `has_name == false`. A multi-file container requires real
// logical names for every file, exactly like the common adapter.
//
// Duplicate logical names within or across containers are rejected through
// the adapter (DIAGNOSTIC_DUPLICATE_SYMBOL), and every failure clears the
// result: a caller must not keep views into it across a failed load.
bool load_descriptor_sets(const std::vector<std::string>& input_paths,
                          SchemaSet& result, Diagnostic& error);

// Refuses a descriptor set whose messages nest deeper than the limit the
// reference compiler applies (easypb_schema::max_message_nesting_depth()),
// BEFORE anything is decoded.
//
// The decode is the recursion that overflows the stack: the runtime's message
// codec descends into every nested message it reads, so a set that nests
// deeper than the limit dies inside the decoder, before any of this project's
// code can look at the bytes. The check therefore runs on the bytes, and the
// walk behind it stops at the limit, so it cannot overflow the stack either.
//
// Returns true when the set is within the limit. On a refusal it fills
// `error` with the reference compiler's own sentence ("Reached maximum
// recursion limit for nested messages.") and names the set in `error.file`.
//
// A set whose bytes the walk cannot read is not refused here: the decode
// meets the same bytes and reports the same problem.
bool check_descriptor_set_nesting(const std::string& bytes,
                                  const std::string& label,
                                  Diagnostic& error);

} // namespace easypb_schema

#endif
