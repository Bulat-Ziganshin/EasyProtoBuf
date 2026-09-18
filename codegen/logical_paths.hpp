#ifndef EASYPB_LOGICAL_PATHS_HPP_INCLUDED
#define EASYPB_LOGICAL_PATHS_HPP_INCLUDED

#include <string>

namespace easypb_schema {

// Pure logical import-name validation shared by every frontend (source
// loader, descriptor-set adapter, future plugin). A logical name is the
// case-sensitive relative path with '/' separators that appears in
// FileDescriptorProto.dependency and in generated include names. This
// module performs no filesystem calls and includes no platform headers,
// so descriptor-only and plugin builds reuse it without file I/O.
//
// A valid logical path:
//   - is not empty;
//   - uses only '/' separators (no backslashes);
//   - is relative (no leading '/', no drive prefix, no ':' at all);
//   - has no empty components (no '//', no leading/trailing '/');
//   - has no '.' or '..' components;
//   - contains no single/double quotes and no control characters
//     (bytes below 0x20 and 0x7F);
//   - is well-formed UTF-8 (no overlong encodings, surrogate code points,
//     truncated sequences, or values above U+10FFFF).
// Separator checks are byte-exact by design: UTF-8 trail bytes never
// collide with ASCII separators, so every 0x5C byte is a backslash.
// Physical paths are a separate concern: Windows uses UTF-8 internally and
// UTF-16 only at W API boundaries, while POSIX keeps native path bytes until
// a disk-derived suffix is promoted into this logical UTF-8 namespace.
bool is_valid_logical_path(const std::string& path);

// Same check with a human-readable reason for diagnostics. Returns true
// when the path is valid; otherwise returns false and stores the reason
// (for example "empty import path" or "import path must not contain '..'").
bool validate_logical_path(const std::string& path, std::string& reason);

} // namespace easypb_schema

#endif
