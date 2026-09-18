#ifndef EASYPB_FILE_PATHS_HPP_INCLUDED
#define EASYPB_FILE_PATHS_HPP_INCLUDED

#include <cstdint>
#include <string>

namespace easypb_file {

// Native physical-path and file-identity helpers for standalone source
// input. This module owns every filesystem call and every platform path
// rule; the common logical-path module and the import graph binder never
// touch the disk. Output support (containment, collisions, directory
// creation) is added in a later plan; this header covers input only.
//
// Physical-path encoding is platform-specific. POSIX physical paths are
// opaque native bytes and pass to OS calls unchanged. Windows physical
// paths use UTF-8 std::string internally and convert strictly to UTF-16
// only at the Win32 W-API boundary; UTF-16 returned by Win32 is converted
// back immediately. Callers of the Windows API therefore must supply
// valid UTF-8. CLI acquisition/transcoding belongs to the later CLI
// integration step, not to this loader. Stored spellings keep their
// original bytes verbatim; comparisons that need character
// semantics (case folding, prefix boundaries, separator recognition)
// convert to wide text internally on Windows without altering them, so
// multibyte sequences are never split. Names with spaces are supported;
// Windows accepts both '/' and '\\' in disk paths while logical names
// always use '/'.

// True when the disk path is absolute on the host platform. On Windows
// this covers drive spellings ("C:/x", "C:x", "C:\\x"), UNC ("\\\\host")
// and rooted ("/x", "\\x") forms; on POSIX it is a leading '/'. Note
// that drive-relative ("C:x") and rooted ("\x") forms are absolute in
// this broad sense but still resolve against per-drive/current
// directories: see is_fully_qualified_disk_path for the narrow form
// that needs no resolution.
bool is_absolute_disk_path(const std::string& path);

// True for fully qualified paths that need no resolution: drive-absolute
// ("C:/x", "C:\\x") and UNC ("\\\\host\\...") on Windows, a leading '/'
// on POSIX. Drive-relative ("C:x") and rooted ("\x", "/x") Windows forms
// resolve against per-drive/current directories and return false here;
// lexical_absolute/absolute_disk_path resolve them to this form first.
// The isolated absolute-root convenience accepts only fully qualified
// paths, never drive-relative spellings.
bool is_fully_qualified_disk_path(const std::string& path);

// Join a search root with a logical import name ("a/b.proto") using the
// native separator convention. A logical name always uses '/'; on
// Windows the result keeps '/' (accepted by the OS) so the logical
// spelling stays visible in diagnostics. A bare Windows drive ("C:") is
// resolved to the drive's current directory first, exactly like
// lexical_absolute, so search and mapping agree on one directory.
std::string join_disk_path(const std::string& directory,
                           const std::string& logical_name);

// True when the path names an existing regular file. Operational
// errors (denied access, I/O failure) report false exactly like a
// missing file; use disk_file_status where the difference matters.
bool disk_file_exists(const std::string& path);

// Filesystem status with operational errors distinguished from
// absence: FILE_STATUS_FILE for an existing regular file,
// FILE_STATUS_ABSENT when nothing usable exists there (missing path,
// directory, or other non-file), FILE_STATUS_ERROR when the probe
// itself fails (denied access, I/O or provider error). Only ABSENT may
// advance a search to the next root; ERROR must stop with a diagnostic
// so a first-root failure can never silently select another file.
// Windows reports ERROR_FILE/PATH_NOT_FOUND as ABSENT and every other
// failure as ERROR; POSIX reports ENOENT/ENOTDIR as ABSENT and every
// other errno as ERROR. The reason is stored in error on ERROR.
enum FileStatus {
    FILE_STATUS_ABSENT,
    FILE_STATUS_FILE,
    FILE_STATUS_ERROR
};
FileStatus disk_file_status(const std::string& path, std::string& error);

// Test seam: while non-null, disk_file_status calls the probe instead
// of touching the filesystem, so error paths stay deterministic
// without changing ACLs. Production code never sets it; tests reset it
// to null when done. Not synchronized: concurrent loads are fine, but
// installing or clearing the probe must not race with a load.
typedef FileStatus (*FileStatusProbe)(const std::string& path,
                                      std::string& error);
void set_file_status_probe(FileStatusProbe probe);

// Read an entire file as bytes. Returns false and stores a short reason
// (including the path) in error on failure.
bool read_disk_file(const std::string& path, std::string& contents,
                    std::string& error);

// Best-effort textual absolute path for diagnostics and prefix mapping.
// On Windows every non-fully-qualified form (relative, drive-relative
// "C:x", rooted "\x") is resolved to a full path so equivalent spellings
// compare equal. This is not proof of physical identity; use FileIdentity
// for caching and alias detection.
std::string absolute_disk_path(const std::string& path);

// Lexically normalized absolute form with '/' separators (backslashes
// become '/' on Windows only), duplicate separators collapsed and
// '.'/'..' resolved lexically without touching the filesystem. Used to
// test whether a disk file lies under a search root before the identity
// check confirms it.
std::string lexical_absolute(const std::string& path);

// Splits a lexical-absolute file path into the remainder under a
// lexical-absolute search root. Returns true and stores the relative
// remainder (never starting with '/') in relative_out when file_absolute
// lies strictly under root_absolute. A root that already ends in '/'
// (filesystem roots such as "/" or UNC shares) matches without an added
// separator, so ("/a.proto", "/") yields "a.proto" rather than "//...".
// Comparison is case-insensitive on Windows, exact elsewhere. The
// Windows comparison runs in wide space and the remainder keeps the
// file's own narrow bytes, so case-equivalent spellings with different
// byte lengths (multibyte characters whose uppercase encodes longer)
// still map without splitting a character. A case-only difference is
// accepted only inside case-insensitive directories: in a
// case-sensitive directory different cases name different files and
// the path is reported outside the root. When operational_error is
// provided, a failed case-sensitivity query stores true there instead
// of silently counting as "outside": callers must turn that state into
// a diagnostic rather than falling through to another root or an
// isolated fallback. A fold-based match is only a candidate: the
// invariant uppercase table is wider than any filesystem's own case
// table, so when used_case_folding is provided it reports whether the
// match relied on case folding at all, and the caller must confirm
// entry-by-entry (confirm_fold_containment) that the root's spelling
// walks the same entries before treating the remainder as contained:
// folding must never merge distinct entries. Returns false when the
// file equals the root itself or lies outside it.
bool strip_search_root_prefix(const std::string& file_absolute,
                              const std::string& root_absolute,
                              std::string& relative_out,
                              bool* operational_error = 0,
                              bool* used_case_folding = 0);

// Confirms a fold-based containment candidate: every leading component
// pair that differs only by case folding must denote the same entry,
// verified by native identity of each spelled prefix (byte-identical
// levels need no query). Unopenable namespace prefixes ("//host")
// stay benign; a bare drive ("C:") is queried as its root. Only
// confirmed absence continues quietly as "not contained"; any other
// query failure stores true in operational_error when provided, so the
// caller turns it into a diagnostic instead of an isolated fallback.
// On POSIX (exact matching, no folding) always true for a genuine
// prefix.
bool confirm_fold_containment(const std::string& root_absolute,
                              const std::string& file_absolute,
                              bool* operational_error = 0);

// True when two absolute spellings walk the same entries at every
// level and therefore denote a single directory entry: byte-identical
// spellings need no query; per-level fold differences are verified
// like confirm_fold_containment, except the final (file) level, where
// the parent directory is enumerated and both spellings must resolve
// to one exact entry name — distinct entries of one file (hardlink
// pairs) are different names, not one spelling. Same doubt policy:
// confirmed absence counts as different, other failures report
// through operational_error.
bool fold_spellings_same_entry(const std::string& left,
                               const std::string& right,
                               bool* operational_error = 0);

// True when two disk-path spellings are textually equivalent: equal
// lexical-absolute forms, compared case-insensitively on Windows and
// exactly elsewhere. On Windows a case-only difference is equivalent
// only inside case-insensitive directories: in a case-sensitive
// directory different cases name different files. Tells equivalent
// isolated-root spellings ("abs/./f.proto", case or separator variants
// on Windows) apart from genuinely different names of one physical
// file (hardlinks/symlinks): only the former deduplicate silently,
// the latter are a duplicate alias. This is textual equivalence only,
// never proof of identity.
bool equivalent_disk_spelling(const std::string& left,
                              const std::string& right);

// Reports whether a directory treats file names case-sensitively:
// 1 when sensitive, 0 when insensitive, -1 when the query API is
// unavailable (then per-directory sensitivity cannot exist), -2 when
// the query itself fails (missing path, denied access, old data).
// Implemented with GetFileInformationByHandleEx resolved at runtime,
// so toolchains without the FileCaseSensitiveInfo declaration still
// build; the directory is opened with metadata rights only. Callers
// keep the historical case-insensitive behavior on -1; only 1 narrows
// a case-insensitive match, while -2 is handled conservatively as a
// rejection.
int directory_case_sensitive(const std::string& directory);

#ifdef _WIN32
// Converts one NUL-terminated Win32 UTF-16 string to the UTF-8 internal
// spelling used by the rest of Codegen. Intended for the process boundary
// (wide argv) and tests; filesystem helpers perform the inverse conversion
// only immediately around W API calls. Returns false for invalid input.
bool utf8_from_wide(const wchar_t* text, std::string& utf8);

// Pure classification used by directory_case_sensitive() and deterministic
// regression tests. These errors mean FileCaseSensitiveInfo itself is not a
// supported capability; all other query failures are operational errors.
bool is_directory_case_query_unsupported_error(unsigned long error_code);

// Deterministic test seam for the four-state case-sensitivity contract.
// Production leaves it null. A non-null probe replaces the Win32 query and
// returns the same 1/0/-1/-2 states as directory_case_sensitive().
typedef int (*DirectoryCaseSensitivityProbe)(const std::string& directory);
void set_directory_case_sensitivity_probe(DirectoryCaseSensitivityProbe probe);
#endif

// Opaque native file identity used for caching and alias detection. Two
// spellings of one file (case alias, symlink, hardlink, "./x" versus
// "x") share one identity; two different files never do. Do not compare
// textual absolute paths instead of this value.
//
// With follow_final_reparse (the default), reparse points on the final
// component are followed: this is the identity of the source content,
// used by the read cache and alias detection. With false, the final
// reparse point is reported itself (Windows OPEN_REPARSE_POINT,
// POSIX lstat): this is the identity of the directory entry, used
// only to confirm that two fold-equivalent spellings traverse the
// same entries. Intermediate components are always traversed.
struct FileIdentity {
    bool valid;
    std::uint64_t key_a;
    std::uint64_t key_b;

    FileIdentity() : valid(false), key_a(0), key_b(0) {}
};

bool get_file_identity(const std::string& path, FileIdentity& identity,
                       std::string& error,
                       bool follow_final_reparse = true);

// Test seam: while non-null, get_file_identity calls the probe instead
// of touching the filesystem, so identity-failure paths stay
// deterministic without changing ACLs. Production code never sets it;
// tests reset it to null when done. Not synchronized: concurrent loads
// are fine, but installing or clearing the probe must not race with
// a load.
typedef bool (*FileIdentityProbe)(const std::string& path,
                                  FileIdentity& identity,
                                  std::string& error);
void set_file_identity_probe(FileIdentityProbe probe);
bool same_identity(const FileIdentity& left, const FileIdentity& right);

// Ordering for associative containers keyed by identity.
bool identity_less(const FileIdentity& left, const FileIdentity& right);

} // namespace easypb_file

#endif
