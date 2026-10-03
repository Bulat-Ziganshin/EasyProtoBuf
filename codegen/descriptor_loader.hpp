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

} // namespace easypb_schema

#endif
