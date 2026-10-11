#ifndef EASYPB_CODEGEN_HEADER_PATHS_HPP_INCLUDED
#define EASYPB_CODEGEN_HEADER_PATHS_HPP_INCLUDED

#include <string>

namespace easypb_codegen {

// Pure mapping from a logical `.proto` name to the generated header name.
//
// A final `.proto` suffix becomes `.pb.hpp`; any other name gains an appended
// `.pb.hpp`. Relative subdirectories and '/' separators are preserved, so
// `common/state.proto` maps to `common/state.pb.hpp` and the result is the
// same spelling an include directive carries and an output root joins below
// itself.
//
// The logical name is validated with the plan-3 logical-path rules (non-empty,
// relative, '/'-separated, no empty/'.'/'..' components, no quotes or control
// bytes, well-formed UTF-8). That validation is exactly what makes the result
// representable in an include directive and safe to join below an output
// root; re-deriving it here would be a second, weaker copy.
//
// This function is pure: no filesystem call, no case folding and no platform
// querying happen here (plan 5 P10). Case aliases and physical collisions are
// the publisher's business. A name that cannot be mapped throws
// std::runtime_error naming the name and the first rule it breaks; callers
// that own a Diagnostic (generate_files) catch it and report it there.
std::string generated_header_path(const std::string& logical_name);

} // namespace easypb_codegen

#endif
