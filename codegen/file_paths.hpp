#ifndef EASYPB_FILE_PATHS_HPP_INCLUDED
#define EASYPB_FILE_PATHS_HPP_INCLUDED

#include <string>
#include <vector>

namespace easypb_file {

// Native physical-path helpers for standalone source input. This module
// owns every filesystem call and every platform path rule; the common
// logical-path module and the import graph binder never touch the disk.
// Output support (containment, collisions, directory creation) is added
// in a later step; this header covers input only.
//
// Physical-path encoding is platform-specific. POSIX physical paths are
// opaque native bytes and pass to OS calls unchanged. Windows physical
// paths use UTF-8 internally and convert strictly to UTF-16 only at the
// Win32 W-API boundary, exactly like the reference compiler's io_win32
// layer; UTF-16 returned by Win32 is converted back immediately. Callers
// on Windows therefore must supply valid UTF-8. CLI acquisition and
// transcoding belong to the later CLI step, not to this loader. Stored
// spellings keep their original bytes verbatim. Names with spaces are
// supported; Windows accepts both '/' and '\' in disk paths while
// logical names always use '/'.
//
// That conversion also applies the long-path prefix of the reference
// layer: the spelling is made absolute, its separators become '\', and
// the result is prefixed with "\\?\" ("\\?\UNC\" for a network share), so
// a filesystem call reaches paths beyond MAX_PATH without a manifest and
// without any registry setting. The prefix turns off Win32 path
// post-processing, so trailing spaces and dots stay literal and '.'/'..'
// are resolved by this module's lexical normalization instead of by the
// API. See wide_win32_path below.
//
// Disk-path handling follows the reference compiler (protoc) on purpose:
// prefix comparison is byte-wise and case-sensitive on every platform,
// and the filesystem itself decides which spellings name which entry.
// There is no case folding and no file-identity query here.

// Join a search root with a logical import name ("a/b.proto") using the
// native separator convention. A logical name always uses '/'; on
// Windows the result keeps '/' (accepted by the OS) so the logical
// spelling stays visible in diagnostics. A bare Windows drive ("C:") is
// resolved to the drive's current directory first, exactly like
// lexical_absolute, so search and mapping agree on one directory.
std::string join_disk_path(const std::string& directory,
                           const std::string& logical_name);

// Filesystem status of one candidate search hit. Only a confirmed
// absence may advance a search to the next root; an inaccessible file
// stops it. The reference compiler draws the line in the same place: a
// denied access is fatal, every other failure counts as absence, and a
// directory is never a usable candidate (protoc's OpenVirtualFile lets
// the search continue when it hits one).
//   - FILE_STATUS_FILE:  the path is an existing regular file;
//   - FILE_STATUS_ABSENT: nothing usable is there (missing path, other
//     non-file, a spelling that is not valid UTF-8, or an unexpected
//     provider failure) and the search may continue;
//   - FILE_STATUS_DIRECTORY: the path is an existing directory. The
//     search treats it like absence, but a command-line argument that
//     names one is reported distinctly instead of "not found";
//   - FILE_STATUS_ERROR: the file exists but cannot be reached (denied
//     access), or the spelling itself is unusable (embedded NUL). The
//     reason is stored in error.
enum FileStatus {
    FILE_STATUS_ABSENT,
    FILE_STATUS_FILE,
    FILE_STATUS_DIRECTORY,
    FILE_STATUS_ERROR
};
FileStatus disk_file_status(const std::string& path, std::string& error);

// Read an entire file as bytes. Returns false and stores a short reason
// (including the path) in error on failure.
bool read_disk_file(const std::string& path, std::string& contents,
                    std::string& error);

// Lexically normalized absolute form with '/' separators (backslashes
// become '/' on Windows only), duplicate separators collapsed and
// '.'/'..' resolved lexically without touching the filesystem. Used to
// test whether a disk file lies under a search root before the file is
// read. This is textual normalization only and never proof of physical
// identity; equivalent spellings of one file do compare equal here.
std::string lexical_absolute(const std::string& path);

// Splits a lexical-absolute file path into the remainder under a
// lexical-absolute search root. Returns true and stores the relative
// remainder (never starting with '/') in relative_out when file_absolute
// lies strictly under root_absolute. A root that already ends in '/'
// (filesystem roots such as "/" or UNC shares) matches without an added
// separator, so ("/a.proto", "/") yields "a.proto" rather than "//...".
// The comparison is byte-wise and case-sensitive, exactly like the
// prefix mapping of the reference compiler: a case variant of a root
// spelling does not map and the file counts as lying outside that root,
// while "." and ".." are never resolved here. Returns false when the
// file equals the root itself or lies outside it.
bool strip_search_root_prefix(const std::string& file_absolute,
                              const std::string& root_absolute,
                              std::string& relative_out);

#ifdef _WIN32
// Converts one NUL-terminated Win32 UTF-16 string to the UTF-8 internal
// spelling used by the rest of Codegen. Intended for the process boundary
// (wide argv) and tests; filesystem helpers perform the inverse conversion
// only immediately around W API calls. Returns false for invalid input.
bool utf8_from_wide(const wchar_t* text, std::string& utf8);

// Converts one UTF-8 disk path to the wide spelling to hand to a Win32 W
// API. The spelling is made absolute through lexical_absolute, its
// separators become '\', and the long-path prefix is prepended - "\\?\"
// for a drive path, "\\?\UNC\" for a network share - exactly like the
// io_win32 layer of the reference compiler. The prefix is what makes a
// path beyond MAX_PATH reachable without a manifest or a registry
// setting, and it also stops the API from post-processing the spelling:
// trailing dots and spaces stay literal, as they are in the stored
// spelling, and '.'/'..' were already resolved lexically. A spelling that
// already carries the prefix is passed through verbatim, and a path that
// cannot be made absolute falls back to the plain conversion this module
// used before long paths were supported; both paths are non-empty
// buffers without a terminator, so callers still append one. Returns
// false only when the spelling is not valid UTF-8 or contains a NUL.
// Exposed for the tests and for later output-side helpers; every call in
// this module that reaches the disk goes through it.
bool wide_win32_path(const std::string& path, std::vector<wchar_t>& wide);
#endif

} // namespace easypb_file

#endif
