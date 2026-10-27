#ifndef EASYPB_FILE_PATHS_HPP_INCLUDED
#define EASYPB_FILE_PATHS_HPP_INCLUDED

#include <string>
#include <vector>

namespace easypb_file {

// Native physical-path helpers for standalone source input. This module
// owns every filesystem call and every platform path rule; the common
// logical-path module and the import graph binder never touch the disk.
// The input and output-side helpers share this module: the output block
// below declares directory creation and the in-place write.
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
//     reason is stored in error, and it names the candidate that was
//     probed (a probe keeps the path; see the action/probe boundary below).
enum FileStatus {
    FILE_STATUS_ABSENT,
    FILE_STATUS_FILE,
    FILE_STATUS_DIRECTORY,
    FILE_STATUS_ERROR
};
FileStatus disk_file_status(const std::string& path, std::string& error);

// Read an entire file as bytes. Returns false and stores a short reason
// without the path in error on failure: the caller names the path it passed
// (the action-entry rule below). A directory at the spelling is reported as
// "is a directory, not a schema file", because both platforms otherwise fold
// it into a denial or a generic read error.
bool read_disk_file(const std::string& path, std::string& contents,
                    std::string& error);

// True when the spelling is absolute in the native sense: a POSIX path
// starting with '/', or a Windows drive-absolute ("C:/x") or UNC
// ("\\host\share") path. A rooted ("\x") or drive-relative ("C:x") Windows
// spelling is not absolute: the platform resolves it against a current
// directory. The loader uses this to require an explicit search root for an
// absolute input (review 1027, U-59).
bool is_absolute_disk_path(const std::string& path);

// True for a Windows drive-relative spelling: a drive letter and ':' with no
// separator after it ("C:foo", "C:"). Such a spelling is anchored to the
// per-drive current directory, so the same text can name a different tree
// from one run to the next; the reference compiler refuses it as a
// --proto_path root (review 1027, U-60). Always false off Windows.
bool is_drive_relative_disk_path(const std::string& path);

// Lexically normalized absolute form with '/' separators (backslashes
// become '/' on Windows only), duplicate separators collapsed and
// '.'/'..' resolved lexically without touching the filesystem. Used to
// test whether a disk file lies under a search root before the file is
// read. Returns an empty string when the input is empty or a relative
// spelling cannot be resolved because the working directory is unknown;
// callers treat that as a refusal, never as the filesystem root. This is
// textual normalization only and never proof of physical identity;
// equivalent spellings of one file do compare equal here.
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
// API. An explicit "\\?\" or "//?/" prefix on a drive-absolute or
// "UNC\server\share" spelling is translated to the plain form first; the
// spelling is then made absolute through lexical_absolute, its separators
// become '\', and the canonical long-path prefix is prepended - "\\?\"
// for a drive path, "\\?\UNC\" for a network share - exactly like the
// io_win32 layer of the reference compiler. The prefix is what makes a
// path beyond MAX_PATH reachable without a manifest or a registry
// setting, and it also stops the API from post-processing the spelling:
// trailing dots and spaces stay literal, as they are in the stored
// spelling, and '.'/'..' were already resolved lexically. A device
// spelling ("\\?\GLOBALROOT\...") is not translated and keeps its
// verbatim passthrough. A relative spelling whose working directory is
// unknown is refused instead of falling back to the relative form; every
// path that is made absolute produces a non-empty buffer without a
// terminator, so callers still append one. Returns false when the
// spelling is not valid UTF-8, contains a NUL, or cannot be made
// absolute. Exposed for the tests and for the output-side helpers;
// every call in this module that reaches the disk goes through it.
bool wide_win32_path(const std::string& path, std::vector<wchar_t>& wide);
#endif

// ---------------------------------------------------------------------------
// Output-side additions (plan 5, step 4: the standalone publisher).
//
// Trimmed to the reference compiler's model by the plan-5 trim (`fbdcc0e`,
// 2026-10-11). The publisher writes files at paths the generator already
// mapped and validated: it creates the parent directories and writes each
// file in place, as protoc's WriteAllToDisk does (TryCreateParentDirectory,
// then open/write/close - command_line_interface.cc:541-610). The output root
// is the one deliberate divergence: protoc verifies that it exists
// (VerifyDirectoryExists), this publisher creates it.
//
// The guards this module used to carry - containment below the output root,
// case-folding collision probing, native file identity, hardlink detection and
// the atomic temp+rename replacement - measured 373 lines and had no
// counterpart in the reference. Their observable effect was measured against
// protoc 30.1 and both divergences are recorded: a descriptor set whose file
// name is "../escape.proto" is refused here through
// easypb_codegen::generated_header_path (the pure logical mapping,
// codegen/header_paths.cpp) and written one directory ABOVE the output
// directory by protoc; and two output names that are one entry on a
// case-folding filesystem silently overwrite here, as they do in protoc.
//
// The loader never needed these and must not have them: what remains of the
// output side - directory creation and the in-place write - belongs to the only
// component that writes an output tree, so it sits beside the input helpers
// instead of inside the common library, which performs no filesystem call at
// all.
//
// Both entries refuse a spelling that contains an embedded NUL on every
// platform, the same rule the read path applies: every native call would
// truncate the spelling at the NUL and address an entry the caller did not
// name. The Windows writer shares the target with concurrent readers and with
// holders that have write access (FILE_SHARE_READ | FILE_SHARE_WRITE, the share
// set the reference's _wopen carries); it does not share delete.
//
// The boundary between an action and a probe: an ACTION entry
// (read_disk_file, make_directories, write_disk_file) returns the reason
// alone and the caller names the path it passed, so a Diagnostic that carries
// the path in its `file` prints it once. A PROBE (disk_file_status) names the
// candidate it probed, because a caller may have several candidates and the
// text has to say which one answered (W-29).
// ---------------------------------------------------------------------------

// Create every missing directory on the way to `directory`; existing
// directories are accepted. Returns false with a reason (without the path:
// the caller names it) on failure, including a directory spelling that cannot
// be made absolute because the working directory is unknown, and a Windows
// network spelling that names a server but no share (`//server`, `//`): such a
// spelling has no directory below it and is never read as a directory under
// the current drive.
//
// `blocking_path`, when given, receives the lexical-absolute spelling of the
// first existing non-directory that blocked the chain ("outfile.bin" when the
// requested root is "outfile.bin/a/b"). It stays empty when the requested
// directory itself is the blocker or when the failure is about the spelling
// rather than a component, so a caller can keep naming the spelling it was
// given. CR-40.
bool make_directories(const std::string& directory, std::string& error,
                      std::string* blocking_path = 0);

// Write `contents` to `target`, creating or truncating it. The target keeps
// either its previous content or the complete new content unless the process
// dies mid-write; a failure removes nothing and reports the reason without
// the path (the caller names it). A directory at `target` is refused as
// `cannot write file (the target is a directory)` on both platforms. This is
// the reference compiler's write model - no temporary file, no rename. A
// target containing an embedded NUL is refused on every platform.
bool write_disk_file(const std::string& target,
                     const std::string& contents,
                     std::string& error);

} // namespace easypb_file

#endif
