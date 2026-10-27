#include "file_paths.hpp"

#include "logical_paths.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {
// A NUL byte never survives a native filesystem call: Win32 W-APIs and
// POSIX c_str() sinks truncate at the first NUL while the narrow
// spelling keeps its full length, so any spelling containing one must
// fail loudly instead of addressing a truncated prefix.
bool has_embedded_nul(const std::string& text)
{
    return text.find('\0') != std::string::npos;
}

// Reason text for a failed read. A permission failure reads as "read
// access is denied" on every platform, the same wording the status query
// uses and the same fact the reference compiler reports for EACCES;
// every other failure keeps its platform error code and text, which is
// still more than the reference compiler prints for them. The caller names
// the path: an action entry returns the reason only (W-29). The POSIX
// branch used to drop the errno value entirely, so every failure read as a
// generic "cannot read file" even though OPTIONS.md promises the platform
// error (review 1027, U-37).
std::string read_error_text(unsigned long code, bool denied)
{
    if (denied) return "read access is denied";
    std::ostringstream message;
    message << "cannot read file";
#ifdef _WIN32
    message << " (Win32 error " << code << ")";
#else
    message << " (errno " << code << ": "
            << std::strerror(static_cast<int>(code)) << ")";
#endif
    return message.str();
}
} // namespace

namespace easypb_file {
namespace {

#ifdef _WIN32
bool is_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// Defined below: narrow to wide conversion and separator normalization.
bool to_wide_text(const std::string& narrow, std::vector<wchar_t>& wide);
bool to_wide_span(const char* data, std::size_t size,
                  std::vector<wchar_t>& wide);
bool wide_to_utf8(const wchar_t* wide, int wide_length, std::string& utf8);
// Appends the NUL terminator Win32 string APIs require. Conversion
// helpers keep exact sizes (wide.size() stays a unit count); only
// buffers handed to W APIs are terminated, right before the call.
void nul_terminate(std::vector<wchar_t>& wide);
std::string normalize_separators(const std::string& path);

// True for fully qualified spellings that need no resolution:
// drive-absolute ("C:/x", "C:\x") and UNC ("\\host\..."). Drive-relative
// ("C:x") and rooted ("\x", "/x") forms resolve against per-drive and
// current directories, so resolve_absolute() maps them through the
// platform path API before prefix comparison.
bool is_fully_qualified_disk_path(const std::string& path)
{
    if (path.size() >= 3 && is_alpha(path[0]) && path[1] == ':' &&
        (path[2] == '/' || path[2] == '\\')) {
        return true;
    }
    if (path.size() >= 2 && (path[0] == '/' || path[0] == '\\') &&
        (path[1] == '/' || path[1] == '\\')) {
        return true;
    }
    return false;
}

// Long-path prefixes, spelled as the reference compiler's io_win32
// layer spells them: "\\?\" states that the rest of the string
// is a path for the object manager, which skips the Win32 parsing that
// both enforces MAX_PATH and "helpfully" trims trailing dots and
// spaces; a network path needs the "\\?\UNC\" variant instead.
const char kLongPathPrefix[] = "\\\\?\\";
const char kLongUncPrefix[] = "\\\\?\\UNC\\";

// Byte-wise test for the prefix. ASCII cannot occur inside a UTF-8
// continuation byte, so the comparison stays exact.
bool has_long_path_prefix(const std::string& path)
{
    const std::size_t length = sizeof(kLongPathPrefix) - 1;
    return path.size() >= length &&
           path.compare(0, length, kLongPathPrefix) == 0;
}

// Recognizes "\\?\" and "//?/" and translates the two bodies this module can
// name into the plain spelling the rest of the path layer uses:
//   \\?\C:\a\b            -> C:/a/b
//   \\?\UNC\host\share\x  -> //host/share/x
// The forward-slash form is the same prefix spelled with '/' (a user may type
// either); "UNC" is case-insensitive, as it is on Windows. Any other body is
// left alone and returns false: a device path is not a file path this module
// can normalize, and the caller keeps its verbatim passthrough.
bool translate_long_path_prefix(const std::string& path, std::string& plain)
{
    plain.clear();
    std::size_t prefix_length = 0;
    const std::size_t backslash_length = sizeof(kLongPathPrefix) - 1;
    if (path.size() >= backslash_length &&
        path.compare(0, backslash_length, kLongPathPrefix) == 0) {
        prefix_length = backslash_length;
    } else if (path.size() >= 4 && path.compare(0, 4, "//?/") == 0) {
        prefix_length = 4;
    } else {
        return false;
    }

    std::string body = path.substr(prefix_length);
    for (std::size_t i = 0; i < body.size(); ++i) {
        if (body[i] == '\\') body[i] = '/';
    }

    // A drive-absolute body: the object manager's "C:" form.
    if (body.size() >= 3 && is_alpha(body[0]) && body[1] == ':' &&
        body[2] == '/') {
        plain.swap(body);
        return true;
    }

    // A network body: "UNC\server\share[\...]". Both the server and the share
    // must be named; a trailing component is optional.
    if (body.size() > 4 && (body[0] == 'U' || body[0] == 'u') &&
        (body[1] == 'N' || body[1] == 'n') &&
        (body[2] == 'C' || body[2] == 'c') && body[3] == '/') {
        const std::size_t host_start = 4;
        const std::size_t host_end = body.find('/', host_start);
        if (host_end != std::string::npos && host_end > host_start) {
            const std::size_t share_end = body.find('/', host_end + 1);
            if (share_end != host_end + 1) {
                plain = "//";
                plain.append(body, host_start, body.size() - host_start);
                return true;
            }
        }
    }
    return false;
}
#endif

// The working directory as a stored spelling. Returns false when the process
// cannot obtain it; a caller must not substitute "." or the root for an answer
// the operating system refused to give.
bool current_working_directory(std::string& result)
{
    result.clear();
#ifdef _WIN32
    // Wide query converted back to UTF-8: the narrow working directory
    // stays in the same encoding as every other path here.
    const DWORD needed = GetCurrentDirectoryW(0, 0);
    if (needed == 0) return false;
    std::vector<wchar_t> buffer(needed);
    const DWORD length = GetCurrentDirectoryW(needed, &buffer[0]);
    if (length == 0 || length >= needed) return false;
    if (!wide_to_utf8(&buffer[0], static_cast<int>(length), result)) {
        return false;
    }
    // A query that cannot answer must not silently become a different
    // directory: an empty conversion means exactly that. Win32 caps the
    // working directory at MAX_PATH in any case - SetCurrentDirectoryW
    // refuses a spelling longer than 258 characters, the prefix included -
    // so this is defence in depth rather than a live case.
    return !result.empty();
#else
    char* cwd = ::getcwd(0, 0);
    if (cwd == 0) return false;
    result.assign(cwd);
    // getcwd with malloc allocation must be freed; free is async-safe
    // here because no other thread touches this pointer.
    ::free(cwd);
    return !result.empty();
#endif
}

bool resolve_with_working_directory(const std::string& path,
                                    std::string& result)
{
    std::string cwd;
    if (!current_working_directory(cwd)) return false;
    result = join_disk_path(cwd, path);
    return true;
}

std::string trim_trailing_separators(const std::string& path)
{
#ifdef _WIN32
    // Keep a bare drive ("C:\") or root ("/", "\", "C:/") intact.
    // Windows narrow paths are strict UTF-8, whose continuation bytes
    // can never equal an ASCII separator, so byte-wise trimming cannot
    // split a multibyte character.
    std::size_t end = path.size();
    while (end > 1 && (path[end - 1] == '/' || path[end - 1] == '\\')) {
        if (end == 3 && path[1] == ':' && is_alpha(path[0])) break;
        --end;
    }
    return path.substr(0, end);
#else
    std::size_t end = path.size();
    while (end > 1 && path[end - 1] == '/') --end;
    return path.substr(0, end);
#endif
}

// Absolute spelling of a disk path without any filesystem query beyond the
// working directory. Returns false when the working directory is unknown and
// a relative spelling needs it. On Windows a relative spelling is joined with
// the working directory and the other non-fully-qualified forms (drive-relative
// "C:x", rooted "\x") are resolved through the platform path API, so equivalent
// spellings compare equal; POSIX prepends the working directory. Textual
// normalization only: it never proves that two spellings name the same entry,
// and no case folding is involved.
bool resolve_absolute(const std::string& path, std::string& result)
{
    result.clear();
    if (path.empty()) return true;
#ifdef _WIN32
    // An explicit long-path prefix is a transport spelling, not part of the
    // name: translate it once, here, so every caller of lexical_absolute
    // (search roots, the output root, comparisons) sees the plain form.
    std::string spelling = path;
    std::string translated;
    if (translate_long_path_prefix(path, translated)) {
        spelling.swap(translated);
    }
    if (is_fully_qualified_disk_path(spelling)) {
        result = spelling;
        return true;
    }
    // A relative spelling is joined textually, the way the reference layer
    // builds its absolute paths. The platform resolver is deliberately not
    // asked here: it trims trailing dots and spaces off the last component,
    // so "dir/leaf.proto." would become "dir/leaf.proto" before the
    // long-path prefix is applied, and one name would silently be answered
    // by a different file. Its own normalization of '.' and '..' is not
    // needed either, because lexical_absolute performs it on every branch.
    if (spelling[0] != '/' && spelling[0] != '\\' &&
        !(spelling.size() >= 2 && is_alpha(spelling[0]) &&
          spelling[1] == ':')) {
        return resolve_with_working_directory(spelling, result);
    }
    // Drive-relative and rooted spellings need the current drive and its
    // per-drive directory, which only the platform resolver knows.
    std::vector<wchar_t> wide;
    if (!to_wide_text(spelling, wide) || wide.empty()) {
        return resolve_with_working_directory(spelling, result);
    }
    nul_terminate(wide);
    // Query the required length first so long paths do not truncate.
    const DWORD needed = GetFullPathNameW(&wide[0], 0, 0, 0);
    if (needed == 0) {
        return resolve_with_working_directory(spelling, result);
    }
    std::vector<wchar_t> buffer(needed);
    const DWORD length =
        GetFullPathNameW(&wide[0], needed, &buffer[0], 0);
    if (length == 0 || length >= needed) {
        return resolve_with_working_directory(spelling, result);
    }
    if (!wide_to_utf8(&buffer[0], static_cast<int>(length), result)) {
        return resolve_with_working_directory(spelling, result);
    }
    return true;
#else
    if (path[0] == '/') {
        result = path;
        return true;
    }
    return resolve_with_working_directory(path, result);
#endif
}

} // namespace

std::string join_disk_path(const std::string& directory,
                           const std::string& logical_name)
{
    if (directory.empty()) return logical_name;
    if (directory == ".") return std::string("./") + logical_name;
#ifdef _WIN32
    // A bare drive ("C:") is drive-relative: it denotes the drive's
    // current directory, not the drive root. Resolve it exactly like
    // lexical_absolute does so search and mapping use one directory.
    // ("C:dir" already keeps its meaning below; only the bare drive
    // needs help. On POSIX "C:" is an ordinary relative name.)
    if (directory.size() == 2 && is_alpha(directory[0]) &&
        directory[1] == ':') {
        const std::string resolved = lexical_absolute(directory);
        if (resolved.empty()) return std::string();
        return resolved + "/" + logical_name;
    }
#endif
    const std::string base = trim_trailing_separators(directory);
    // trim keeps root separators ("/", "\", "C:/") intact; do not add a
    // second one or "/" becomes the UNC path "//...".
    if (!base.empty() && base[base.size() - 1] == '/') {
        return base + logical_name;
    }
#ifdef _WIN32
    if (!base.empty() && base[base.size() - 1] == '\\') {
        return base + logical_name;
    }
#endif
    return base + "/" + logical_name;
}

bool is_absolute_disk_path(const std::string& path)
{
    if (path.empty()) return false;
#ifdef _WIN32
    return is_fully_qualified_disk_path(path);
#else
    return path[0] == '/';
#endif
}

bool is_drive_relative_disk_path(const std::string& path)
{
#ifdef _WIN32
    return path.size() >= 2 && is_alpha(path[0]) && path[1] == ':' &&
           (path.size() == 2 ||
            (path[2] != '/' && path[2] != '\\'));
#else
    (void)path;
    return false;
#endif
}

FileStatus disk_file_status(const std::string& path, std::string& error)
{
    error.clear();
    if (path.empty()) return FILE_STATUS_ABSENT;
    if (has_embedded_nul(path)) {
        // Native calls would see only the truncated prefix: only ERROR
        // may result, never a match for a different spelling.
        error = path + ": path contains NUL character";
        return FILE_STATUS_ERROR;
    }
#ifdef _WIN32
    std::vector<wchar_t> wide;
    // Long-path-aware conversion: a candidate below a deep root is
    // queried under its "\\?\" spelling, so the query itself never
    // fails merely because the tree is longer than MAX_PATH. A spelling
    // that cannot be converted (undecodable bytes, embedded NUL) cannot
    // name a file: the reference compiler's Windows layer reports ENOENT
    // for it, so the search advances, and the spelling is rejected later
    // if it also fails the logical name rules.
    if (!wide_win32_path(path, wide) || wide.empty()) {
        return FILE_STATUS_ABSENT;
    }
    nul_terminate(wide);
    const DWORD attributes = GetFileAttributesW(&wide[0]);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND ||
            code == ERROR_INVALID_NAME) {
            return FILE_STATUS_ABSENT;
        }
        if (code == ERROR_ACCESS_DENIED) {
            error = path + ": read access is denied";
            return FILE_STATUS_ERROR;
        }
        // Anything else counts as absence: a failure that is not a
        // denial never masks another file behind it.
        return FILE_STATUS_ABSENT;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        // A directory is never a schema file, but it is also not an
        // ordinary miss: the caller decides whether that matters.
        return FILE_STATUS_DIRECTORY;
    }
    return FILE_STATUS_FILE;
#else
    struct stat status;
    if (::stat(path.c_str(), &status) != 0) {
        const int code = errno;
        if (code == ENOENT || code == ENOTDIR) return FILE_STATUS_ABSENT;
        if (code == EACCES) {
            error = path + ": read access is denied";
            return FILE_STATUS_ERROR;
        }
        return FILE_STATUS_ABSENT;
    }
    if (S_ISDIR(status.st_mode) != 0) return FILE_STATUS_DIRECTORY;
    return S_ISREG(status.st_mode) != 0 ? FILE_STATUS_FILE
                                        : FILE_STATUS_ABSENT;
#endif
}

// A failed read of a directory is a directory, not a denied or unreadable
// file: Windows answers ERROR_ACCESS_DENIED for CreateFileW on a directory
// opened without FILE_FLAG_BACKUP_SEMANTICS, and POSIX opens it and fails the
// read with EISDIR. The probe runs only after a read has failed, so the
// success path pays nothing. This is what lets the utility modes report
// "is a directory, not a schema file" instead of a shared "cannot read file"
// (CR-37/CR-47).
namespace {

std::string read_failure_reason(const std::string& path,
                                const std::string& reason)
{
    std::string status_error;
    if (disk_file_status(path, status_error) == FILE_STATUS_DIRECTORY) {
        return "is a directory, not a schema file";
    }
    return reason;
}

} // namespace

bool read_disk_file(const std::string& path, std::string& contents,
                    std::string& error)
{
    if (has_embedded_nul(path)) {
        error = "path contains NUL character";
        return false;
    }
#ifdef _WIN32
    // Wide, long-path-aware open of the UTF-8 spelling: files outside the
    // process code page or below a path longer than MAX_PATH stay
    // addressable, exactly like the reference compiler's io_win32 layer.
    std::vector<wchar_t> wide;
    if (!wide_win32_path(path, wide) || wide.empty()) {
        error = "cannot read file";
        return false;
    }
    nul_terminate(wide);
    HANDLE handle = CreateFileW(&wide[0], GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE |
                                    FILE_SHARE_DELETE,
                                0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        error = read_failure_reason(
            path, read_error_text(code, code == ERROR_ACCESS_DENIED));
        return false;
    }
    contents.clear();
    char buffer[8192];
    for (;;) {
        DWORD chunk = 0;
        if (!ReadFile(handle, buffer, sizeof(buffer), &chunk, 0)) {
            const DWORD code = GetLastError();
            error = read_failure_reason(
                path, read_error_text(code, code == ERROR_ACCESS_DENIED));
            CloseHandle(handle);
            return false;
        }
        if (chunk == 0) break;
        contents.append(buffer, chunk);
    }
    CloseHandle(handle);
    return true;
#else
    // Open once and read through the descriptor. The stream-based read this
    // replaces could not report a failure: istreambuf_iterator reads through
    // the stream buffer and never calls setstate(), so the post-read fail()
    // guard was dead code and a short read returned truncated data with
    // success. The stat probe also ran before the open, leaving a window in
    // which the entry could be replaced (review 1027, U-27). fstat answers
    // for the entry that was actually opened, and the loop treats every
    // short count as progress rather than as the end.
    errno = 0;
    const int descriptor = ::open(path.c_str(), O_RDONLY);
    if (descriptor < 0) {
        const int code = errno;
        error = read_failure_reason(
            path, read_error_text(static_cast<unsigned long>(code),
                                  code == EACCES));
        return false;
    }
    struct stat status;
    if (::fstat(descriptor, &status) != 0) {
        const int code = errno;
        ::close(descriptor);
        error = read_failure_reason(
            path, read_error_text(static_cast<unsigned long>(code),
                                  code == EACCES));
        return false;
    }
    if (S_ISDIR(status.st_mode) != 0) {
        ::close(descriptor);
        error = "is a directory, not a schema file";
        return false;
    }
    contents.clear();
    // The size is a hint only: the loop below never trusts it, so a file
    // that grows or shrinks between fstat and read still reads exactly once.
    // The reservation is capped so a sparse file cannot ask for a giant
    // buffer up front.
    if (status.st_size > 0) {
        const std::size_t hint = static_cast<std::size_t>(status.st_size);
        contents.reserve(hint < (1u << 20) ? hint : (1u << 20));
    }
    char buffer[8192];
    for (;;) {
        const ssize_t chunk = ::read(descriptor, buffer, sizeof(buffer));
        if (chunk < 0) {
            if (errno == EINTR) continue;
            const int code = errno;
            ::close(descriptor);
            contents.clear();
            error = read_failure_reason(
                path, read_error_text(static_cast<unsigned long>(code),
                                      code == EACCES));
            return false;
        }
        if (chunk == 0) break;
        contents.append(buffer, static_cast<std::size_t>(chunk));
    }
    ::close(descriptor);
    return true;
#endif
}

std::string lexical_absolute(const std::string& path)
{
    if (path.empty()) return std::string();
    std::string absolute;
    if (!resolve_absolute(path, absolute)) return std::string();
#ifdef _WIN32
    absolute = normalize_separators(absolute);
#endif
    // Split off the Windows drive/UNC prefix so ".." never climbs above it.
    // Positional byte checks below stay exact because ASCII '/' cannot
    // occur inside a valid UTF-8 multibyte sequence.
    std::string prefix;
    std::string rest = absolute;
#ifdef _WIN32
    if (rest.size() >= 2 && is_alpha(rest[0]) && rest[1] == ':') {
        prefix = rest.substr(0, 2);
        rest.erase(0, 2);
    } else if (rest.size() >= 2 && rest[0] == '/' && rest[1] == '/') {
        // A UNC spelling is "//server/share" plus the part below the share; a
        // server on its own ("//server", "//") is still a network spelling.
        // Both keep their prefix. Folding the server-only form into the rooted
        // "/server" made every later step read it as a directory under the
        // current drive (measured: `--out-dir //localhost` created C:\localhost
        // and only then failed with ERROR_BAD_NET_NAME).
        const std::size_t host_end = rest.find('/', 2);
        if (host_end != std::string::npos) {
            const std::size_t share_end = rest.find('/', host_end + 1);
            if (share_end != std::string::npos) {
                prefix = rest.substr(0, share_end);
                rest.erase(0, share_end);
            } else {
                prefix = rest;
                rest.clear();
            }
        } else {
            prefix = rest;
            rest.clear();
        }
    }
    if (!rest.empty() && rest[0] != '/') rest = "/" + rest;
#else
    if (!rest.empty() && rest[0] != '/') rest = "/" + rest;
#endif

    std::vector<std::string> components;
    std::size_t start = 0;
    while (start < rest.size()) {
        while (start < rest.size() && rest[start] == '/') ++start;
        if (start >= rest.size()) break;
        std::size_t end = rest.find('/', start);
        if (end == std::string::npos) end = rest.size();
        const std::string component = rest.substr(start, end - start);
        start = end;
        if (component.empty() || component == ".") continue;
        if (component == "..") {
            if (!components.empty()) components.pop_back();
            continue;
        }
        components.push_back(component);
    }

    std::string result = prefix;
    for (std::size_t i = 0; i < components.size(); ++i) {
        result += "/";
        result += components[i];
    }
    if (result.empty()) {
#ifdef _WIN32
        if (!prefix.empty()) return prefix + "/";
        // The only way here with an empty prefix is a non-absolute absolute
        // form; ask once more rather than inventing a root. A failed query is
        // the same refusal as above.
        std::string cwd;
        return current_working_directory(cwd) ? cwd : std::string();
#else
        return std::string("/");
#endif
    }
#ifdef _WIN32
    // A bare drive prefix with no surviving components ("C:/",
    // "C:/dir/..") denotes the drive root: keep its slash instead of
    // degrading to the drive-relative "C:", which resolves against the
    // per-drive current directory once composed further.
    if (components.empty() && prefix.size() == 2 && prefix[1] == ':') {
        return prefix + "/";
    }
#endif
    return result;
}

#ifdef _WIN32
bool wide_win32_path(const std::string& path, std::vector<wchar_t>& wide)
{
    wide.clear();
    if (path.empty()) return true;

    // An explicit long-path prefix is translated to its plain spelling before
    // this layer re-applies the canonical prefix below. A recognized device
    // path keeps the old verbatim passthrough: the translator accepts only
    // the two bodies this module can name (a drive path and a UNC share).
    std::string spelling = path;
    std::string translated;
    if (translate_long_path_prefix(path, translated)) {
        spelling.swap(translated);
    } else if (has_long_path_prefix(path)) {
        return to_wide_text(path, wide);
    }

    // The prefix bypasses Win32 path parsing, so nothing may be left for
    // that parsing to do: a relative, drive-relative or rooted spelling is
    // resolved to a fully qualified one first, and '.' and '..' are removed
    // lexically because the API no longer interprets them. A relative
    // spelling whose working directory is unknown cannot be made absolute:
    // that is a refusal, not a fallback to the original spelling.
    const std::string absolute = lexical_absolute(spelling);
    if (absolute.empty()) return false;

    const bool unc =
        absolute.size() >= 2 && absolute[0] == '/' && absolute[1] == '/';
    const bool drive_absolute =
        absolute.size() >= 3 && is_alpha(absolute[0]) &&
        absolute[1] == ':' && absolute[2] == '/';
    if (!unc && !drive_absolute) {
        // Neither form is prefixable (a bare drive): keep the plain
        // conversion rather than hand Win32 a prefix it would reject.
        return to_wide_text(spelling, wide);
    }

    // The prefix is applied to every path, not only to long ones: it is
    // also what stops Win32 from "helpfully" trimming trailing dots and
    // spaces before the entry is looked up, which is why the reference
    // compiler's io_win32 layer is unconditional about it as well
    // (bazelbuild/bazel#2935). The one form that layer cannot express is a
    // network path, whose leading "\\" its normalization drops: a UNC
    // spelling takes the "\\?\UNC\" variant here, the only spelling that
    // keeps it reachable with a prefix.
    std::string body = absolute;
    std::string prefixed;
    if (unc) {
        prefixed.assign(kLongUncPrefix);
        body.erase(0, 2);
    } else {
        prefixed.assign(kLongPathPrefix);
    }
    // Separators become '\' because the object manager does not accept '/'
    // after the prefix. UTF-8 continuation bytes can never equal ASCII '/',
    // so the pass stays byte-wise and no multibyte name changes.
    for (std::size_t i = 0; i < body.size(); ++i) {
        prefixed += body[i] == '/' ? '\\' : body[i];
    }
    return to_wide_text(prefixed, wide);
}
#endif

bool strip_search_root_prefix(const std::string& file_absolute,
                              const std::string& root_absolute,
                              std::string& relative_out)
{
    // A root that already ends in '/' needs no added separator.
    std::string prefix = root_absolute;
    if (!prefix.empty() && prefix[prefix.size() - 1] != '/') prefix += "/";
    if (prefix.empty()) {
        relative_out = file_absolute;
        return !relative_out.empty();
    }
    // Byte-wise comparison with a component boundary, exactly like the
    // reference compiler's ApplyMapping: case-sensitive on every
    // platform, and a root spelled differently does not contain the
    // file. '.' and '..' are already resolved by lexical_absolute.
    if (file_absolute.size() < prefix.size() ||
        file_absolute.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    relative_out = file_absolute.substr(prefix.size());
    return !relative_out.empty();
}

#ifdef _WIN32
bool utf8_from_wide(const wchar_t* text, std::string& utf8)
{
    utf8.clear();
    if (text == 0) return false;
    const int length = lstrlenW(text);
    return wide_to_utf8(text, length, utf8);
}
#endif

namespace {

#ifdef _WIN32
// Converts one narrow (UTF-8, the single narrow encoding of this
// module) string to wide characters. Returns false when the spelling
// is not valid UTF-8.
bool to_wide_text(const std::string& narrow, std::vector<wchar_t>& wide)
{
    return to_wide_span(narrow.data(), narrow.size(), wide);
}

// Converts a byte range without allocating a substring.
void nul_terminate(std::vector<wchar_t>& wide)
{
    wide.push_back(L'\0');
}

bool to_wide_span(const char* data, std::size_t size,
                  std::vector<wchar_t>& wide){
    wide.clear();
    if (size == 0) return true;
    // U+0000 is valid UTF-8 and would decode cleanly, but every
    // NUL-terminated Win32 call below would truncate at the first one
    // while the narrow spelling keeps its full length: reject up front.
    if (std::memchr(data, '\0', size) != 0) return false;
    const int need =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data,
                            static_cast<int>(size), 0, 0);
    if (need <= 0) return false;
    wide.resize(static_cast<std::size_t>(need));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data,
                               static_cast<int>(size), &wide[0],
                               need) == need;
}

// Converts wide characters back to narrow UTF-8. Returns false when
// the input is not well-formed UTF-16 or the conversion otherwise fails.
// Validate surrogate pairing ourselves rather than relying on
// WC_ERR_INVALID_CHARS, which older Windows versions do not accept.
bool wide_to_utf8(const wchar_t* wide, int wide_length, std::string& utf8)
{
    utf8.clear();
    if (wide_length <= 0) return true;
    for (int i = 0; i < wide_length; ++i) {
        const unsigned int unit = static_cast<unsigned int>(
            static_cast<unsigned short>(wide[i]));
        if (unit >= 0xD800u && unit <= 0xDBFFu) {
            if (i + 1 >= wide_length) return false;
            const unsigned int low = static_cast<unsigned int>(
                static_cast<unsigned short>(wide[i + 1]));
            if (low < 0xDC00u || low > 0xDFFFu) return false;
            ++i;
        } else if (unit >= 0xDC00u && unit <= 0xDFFFu) {
            return false;
        }
    }
    const int need =
        WideCharToMultiByte(CP_UTF8, 0, wide, wide_length, 0, 0, 0, 0);
    if (need <= 0) return false;
    std::vector<char> buffer(static_cast<std::size_t>(need));
    const int done =
        WideCharToMultiByte(CP_UTF8, 0, wide, wide_length, &buffer[0],
                            need, 0, 0);
    if (done != need) return false;
    utf8.assign(&buffer[0], static_cast<std::size_t>(need));
    return true;
}

// Rewrites every real backslash as '/'. Windows narrow paths are UTF-8,
// whose continuation bytes can never equal ASCII '\\', so separator
// recognition is unambiguous and the pass may stay byte-wise.
std::string normalize_separators(const std::string& path)
{
    std::string result = path;
    for (std::size_t i = 0; i < result.size(); ++i) {
        if (result[i] == '\\') result[i] = '/';
    }
    return result;
}
#endif

} // namespace

// ---------------------------------------------------------------------------
// Output-side helpers (plan 5 batch 3: the standalone publisher).
//
// They live in the same module as the input helpers because they are the
// same kind of knowledge - what the filesystem does with a spelling - and
// because the common schema library must keep performing no filesystem call
// at all. Nothing here is used by the loader; the publisher is their only
// caller besides the tests.
//
// Trimmed to the reference compiler's model by the plan-5 trim (`fbdcc0e`,
// 2026-10-11). What is left is the directory tree and the write itself, which
// is what protoc's WriteAllToDisk does (command_line_interface.cc:541-610):
// create the parent directories, then open/write/close. The output root is
// created when missing here where protoc verifies it. The containment, case
// folding, native identity, hardlink and atomic temp+rename guards that used to
// live here are gone, with their measured effect recorded in file_paths.hpp.
// ---------------------------------------------------------------------------

namespace {


std::string number_text(unsigned long long code)
{
    std::ostringstream message;
    message << code;
    return message.str();
}

// After a failed open, a directory at the target is its own failure: Win32
// reports ERROR_ACCESS_DENIED and POSIX EISDIR, and neither sentence says
// what is actually there. The check runs only on the failure path, so no
// successful write pays for an extra query (W-29).
bool write_target_is_directory(const std::string& target)
{
    std::string ignored;
    return disk_file_status(target, ignored) == FILE_STATUS_DIRECTORY;
}

// The two messages below are the Win32 ones. The POSIX write and the POSIX
// directory creation build their own text from strerror, so these helpers
// have no caller there: they live in the Windows guard for that reason.
// Outside it they are dead code, and a dead function is a warning in every
// POSIX build (measured: the gcc and clang lanes of 2026-10-22).
#ifdef _WIN32
std::string write_error_text(unsigned long long code)
{
    return "cannot write file (Win32 error " + number_text(code) + ")";
}
std::string create_directory_error_text(unsigned long long code)
{
    return "cannot create directory (Win32 error " + number_text(code) + ")";
}
#endif

// The length of the part of an absolute path that the filesystem provides and
// this module never creates: the drive root ("C:/"), the UNC share
// ("//server/share/") or the filesystem root ("/"). Everything below it is the
// chain of directories the caller asks for.
//
// A UNC share is created by its server, so only what lies below the share is
// this function's business; a path that names the share and nothing else has
// no components to create.
std::size_t root_prefix_length(const std::string& absolute)
{
#ifdef _WIN32
    if (absolute.size() >= 3 && is_alpha(absolute[0]) &&
        absolute[1] == ':' && absolute[2] == '/') {
        return 3;
    }
    if (absolute.size() >= 2 && absolute[0] == '/' && absolute[1] == '/') {
        const std::size_t server_end = absolute.find('/', 2);
        if (server_end == std::string::npos) return absolute.size();
        const std::size_t share_end = absolute.find('/', server_end + 1);
        if (share_end == std::string::npos) return absolute.size();
        return share_end + 1;
    }
#endif
    if (!absolute.empty() && absolute[0] == '/') return 1;
    return 0;
}

#ifdef _WIN32
// True for a network spelling that names a server but no share: "//server",
// "//server/" and "//" (also with extra separators). Such a spelling has no
// directory below it, so there is nothing to create, and it must never be
// read as the rooted "/server" under the current drive: that is how
// `--out-dir //localhost` used to create C:\localhost (CR-32).
bool is_server_only_unc(const std::string& absolute)
{
    if (absolute.size() < 2 || absolute[0] != '/' || absolute[1] != '/')
        return false;
    bool server_seen = false;
    std::size_t start = 2;
    while (start < absolute.size()) {
        while (start < absolute.size() && absolute[start] == '/') ++start;
        if (start >= absolute.size()) break;
        std::size_t end = absolute.find('/', start);
        if (end == std::string::npos) end = absolute.size();
        // The first component is the server; a second one is the share, and a
        // share root is an addressable directory.
        if (server_seen) return false;
        server_seen = true;
        start = end;
    }
    return true;
}
#endif

// W-52: file-local by use. create_directory_plain's only caller is
// make_directories_recursive, and that function's only caller is the public
// make_directories, so both keep internal linkage instead of publishing two
// declarations no header needs.
bool create_directory_plain(const std::string& path, std::string& error)
{
#ifdef _WIN32
    std::vector<wchar_t> wide;
    if (!wide_win32_path(path, wide) || wide.empty()) {
        error = "cannot create directory";
        return false;
    }
    nul_terminate(wide);
    if (CreateDirectoryW(&wide[0], 0)) return true;
    const DWORD code = GetLastError();
    if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS) {
        // "Already there" is only success when what is there is a directory;
        // an existing FILE at this spelling is the classic denied output root
        // (plan 5 P8), not a no-op.
        std::string status_error;
        if (disk_file_status(path, status_error) == FILE_STATUS_DIRECTORY) {
            return true;
        }
        error = "output path is an existing file, not a directory";
        return false;
    }
    error = create_directory_error_text(code);
    return false;
#else
    if (::mkdir(path.c_str(), 0777) == 0) return true;
    if (errno == EEXIST) {
        std::string status_error;
        if (disk_file_status(path, status_error) == FILE_STATUS_DIRECTORY) {
            return true;
        }
        error = "output path is an existing file, not a directory";
        return false;
    }
    const int code = errno;
    error = "cannot create directory (" + std::string(std::strerror(code)) + ")";
    return false;
#endif
}

// Create every missing directory on the way to `absolute`.
//
// The chain is created from the root down, one component at a time, so the
// call stack does not grow with the depth of the path. The recursion this
// replaces walked the same chain from the bottom up and copied the whole
// remaining path in every frame, which made the work grow with the square of
// the depth and ended the process with a stack overflow on a path deep
// enough.
//
// The number of components is bounded (the same bound the logical path rule
// applies, easypb_schema::max_logical_path_components), and the count is
// taken before the first directory is created: a refused path leaves no tree
// behind.
bool make_directories_recursive(const std::string& absolute, std::string& error,
                                std::string* blocking_path)
{
    if (absolute.empty() || absolute == "/") return true;

    const std::size_t root_length = root_prefix_length(absolute);

    // The components below the root, as the offset of their end.
    std::vector<std::size_t> ends;
    std::size_t start = root_length;
    while (start < absolute.size()) {
        std::size_t end = absolute.find('/', start);
        if (end == std::string::npos) end = absolute.size();
        if (end > start) ends.push_back(end);
        if (end == absolute.size()) break;
        start = end + 1;
    }

    const std::size_t limit = easypb_schema::max_logical_path_components();
    if (ends.size() > limit) {
        error = "cannot create directory (the path has " +
                number_text(static_cast<unsigned long long>(ends.size())) +
                " components, more than the " + number_text(limit) +
                " this tool accepts)";
        return false;
    }

    std::string path = absolute.substr(0, root_length);
    std::size_t component_start = root_length;
    for (std::size_t i = 0; i < ends.size(); ++i) {
        if (!path.empty() && path[path.size() - 1] != '/') path.push_back('/');
        path.append(absolute, component_start, ends[i] - component_start);
        component_start = ends[i] + 1;
        if (!create_directory_plain(path, error)) {
            // The reason is about this component, not about the requested
            // path: name the component that answered so the caller can label
            // the diagnostic with it (CR-40).
            if (blocking_path != 0) *blocking_path = path;
            return false;
        }
    }
    return true;
}

} // namespace

bool make_directories(const std::string& directory, std::string& error,
                      std::string* blocking_path)
{
    error.clear();
    if (blocking_path != 0) blocking_path->clear();
    if (directory.empty()) {
        error = "cannot create an empty directory path";
        return false;
    }
    if (has_embedded_nul(directory)) {
        error = "path contains NUL character";
        return false;
    }
    const std::string absolute = lexical_absolute(directory);
    if (absolute.empty()) {
        error = "cannot resolve directory (the current working directory "
                "is unknown)";
        return false;
    }
#ifdef _WIN32
    // A server-only network spelling has no directory to create, and letting
    // it through would create one under the current drive (CR-32). protoc's
    // equivalent open fails with "No such file or directory"; this refuses the
    // spelling before anything exists.
    if (is_server_only_unc(absolute)) {
        error = "cannot create directory (a network path names a server but "
                "no share)";
        return false;
    }
#endif
    if (!make_directories_recursive(absolute, error, blocking_path)) {
        // When the requested directory itself is the file, the caller's own
        // spelling is the right label (and keeps a relative spelling
        // relative). Only a component above it is news to the caller.
        if (blocking_path != 0 && *blocking_path == absolute) {
            blocking_path->clear();
        }
        return false;
    }
    return true;
}

// Write `contents` to `target`, creating or truncating it. The reference
// compiler's write model (command_line_interface.cc:559-610): open with
// O_CREAT | O_TRUNC, write the whole buffer, close. No temporary file, no
// rename, and therefore no "never a prefix of the new content" promise - a
// process killed mid-write can leave a truncated header, exactly as protoc
// can.
bool write_disk_file(const std::string& target, const std::string& contents,
                     std::string& error)
{
    error.clear();
    if (has_embedded_nul(target)) {
        // The same rule the read path applies: a NUL never survives a native
        // call, and truncating it would write at a different spelling than
        // the caller named. Refuse before either platform branch.
        error = "path contains NUL character";
        return false;
    }
#ifdef _WIN32
    std::vector<wchar_t> wide;
    if (!wide_win32_path(target, wide) || wide.empty()) {
        error = "cannot write file";
        return false;
    }
    nul_terminate(wide);
    // Share read and write, exactly as the reference's _wopen does (the CRT
    // default is _SH_DENYNO): a concurrent reader that permits a writer, or
    // another holder with write access, must not block the replacement.
    // Delete is not shared, matching the reference.
    HANDLE handle = CreateFileW(&wide[0], GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (handle == INVALID_HANDLE_VALUE) {
        // The create's code must be read before any other Win32 call: the
        // directory probe below queries attributes and overwrites the
        // thread's last-error value, which used to replace "access denied"
        // with the probe's "not found" (measured: a denied create reported
        // Win32 error 2 where the create said 5). The POSIX branch has always
        // captured errno first.
        const DWORD code = GetLastError();
        // A directory answers ERROR_ACCESS_DENIED on Windows; name the
        // condition instead of the generic code (W-29).
        if (write_target_is_directory(target)) {
            error = "cannot write file (the target is a directory)";
            return false;
        }
        error = write_error_text(code);
        return false;
    }
    std::size_t done = 0;
    while (done < contents.size()) {
        const std::size_t chunk =
            std::min<std::size_t>(contents.size() - done, 1u << 20);
        DWORD written = 0;
        if (!WriteFile(handle, contents.data() + done,
                       static_cast<DWORD>(chunk), &written, 0) ||
            written != chunk) {
            const DWORD code = GetLastError();
            CloseHandle(handle);
            error = write_error_text(code);
            return false;
        }
        done += chunk;
    }
    if (!CloseHandle(handle)) {
        error = write_error_text(GetLastError());
        return false;
    }
    return true;
#else
    // Retry an interrupted open, as the reference's WriteAllToDisk does.
    int descriptor = -1;
    do {
        descriptor =
            ::open(target.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        const int code = errno;
        // A directory answers EISDIR; name the condition (W-29).
        if (write_target_is_directory(target)) {
            error = "cannot write file (the target is a directory)";
            return false;
        }
        error = "cannot write file (" + std::string(std::strerror(code)) + ")";
        return false;
    }
    // One bounded request at a time: the cap changes no output byte, only
    // the size of one syscall. EINTR is not a failure, and a zero return is
    // not progress (the old loop repeated forever on it).
    const std::size_t chunk_limit = 64u * 1024u;
    std::size_t done = 0;
    while (done < contents.size()) {
        const std::size_t remaining = contents.size() - done;
        const std::size_t chunk =
            remaining < chunk_limit ? remaining : chunk_limit;
        ssize_t written = -1;
        do {
            written = ::write(descriptor, contents.data() + done, chunk);
        } while (written < 0 && errno == EINTR);
        if (written < 0) {
            const int code = errno;
            ::close(descriptor);
            error = "cannot write file (" + std::string(std::strerror(code)) + ")";
            return false;
        }
        if (written == 0) {
            ::close(descriptor);
            error = "cannot write file (the write made no progress)";
            return false;
        }
        done += static_cast<std::size_t>(written);
    }
    if (::close(descriptor) != 0) {
        const int code = errno;
        error = "cannot write file (" + std::string(std::strerror(code)) + ")";
        return false;
    }
    return true;
#endif
}


} // namespace easypb_file
