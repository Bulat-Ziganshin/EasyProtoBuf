#include "file_paths.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
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
// every other failure keeps its platform error code, which is still more
// than the reference compiler prints for them.
std::string read_error_text(const std::string& path, unsigned long code,
                            bool denied)
{
    if (denied) return path + ": read access is denied";
    std::ostringstream message;
    message << path << ": cannot read file";
#ifdef _WIN32
    message << " (Win32 error " << code << ")";
#else
    (void)code;
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
#endif

std::string current_working_directory()
{
#ifdef _WIN32
    // Wide query converted back to UTF-8: the narrow working directory
    // stays in the same encoding as every other path here.
    const DWORD needed = GetCurrentDirectoryW(0, 0);
    if (needed == 0) return std::string(".");
    std::vector<wchar_t> buffer(needed);
    const DWORD length = GetCurrentDirectoryW(needed, &buffer[0]);
    if (length == 0 || length >= needed) return std::string(".");
    std::string result;
    if (!wide_to_utf8(&buffer[0], static_cast<int>(length), result)) {
        return std::string(".");
    }
    // A query that cannot answer must not silently become a different
    // directory: an empty conversion means exactly that. Win32 caps the
    // working directory at MAX_PATH in any case - SetCurrentDirectoryW
    // refuses a spelling longer than 258 characters, the prefix included -
    // so this is defence in depth rather than a live case.
    if (result.empty()) return std::string(".");
    return result;
#else
    char* cwd = ::getcwd(0, 0);
    if (cwd == 0) return std::string(".");
    const std::string result(cwd);
    // getcwd with malloc allocation must be freed; free is async-safe
    // here because no other thread touches this pointer.
    ::free(cwd);
    return result;
#endif
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

// Absolute spelling of a disk path without any filesystem query. On
// Windows a relative spelling is joined with the working directory and the
// other non-fully-qualified forms (drive-relative "C:x", rooted "\x") are
// resolved through the platform path API, so equivalent spellings compare
// equal; POSIX prepends the working directory. Textual normalization only:
// it never proves that two spellings name the same entry, and no case
// folding is involved.
std::string resolve_absolute(const std::string& path)
{
    if (path.empty()) return path;
#ifdef _WIN32
    if (is_fully_qualified_disk_path(path)) return path;
    // A relative spelling is joined textually, the way the reference layer
    // builds its absolute paths. The platform resolver is deliberately not
    // asked here: it trims trailing dots and spaces off the last component,
    // so "dir/leaf.proto." would become "dir/leaf.proto" before the
    // long-path prefix is applied, and one name would silently be answered
    // by a different file. Its own normalization of '.' and '..' is not
    // needed either, because lexical_absolute performs it on every branch.
    if (path[0] != '/' && path[0] != '\\' &&
        !(path.size() >= 2 && is_alpha(path[0]) && path[1] == ':')) {
        return join_disk_path(current_working_directory(), path);
    }
    // Drive-relative and rooted spellings need the current drive and its
    // per-drive directory, which only the platform resolver knows.
    std::vector<wchar_t> wide;
    if (!to_wide_text(path, wide) || wide.empty()) {
        return join_disk_path(current_working_directory(), path);
    }
    nul_terminate(wide);
    // Query the required length first so long paths do not truncate.
    const DWORD needed = GetFullPathNameW(&wide[0], 0, 0, 0);
    if (needed == 0) {
        return join_disk_path(current_working_directory(), path);
    }
    std::vector<wchar_t> buffer(needed);
    const DWORD length =
        GetFullPathNameW(&wide[0], needed, &buffer[0], 0);
    if (length == 0 || length >= needed) {
        return join_disk_path(current_working_directory(), path);
    }
    std::string result;
    if (!wide_to_utf8(&buffer[0], static_cast<int>(length), result)) {
        return join_disk_path(current_working_directory(), path);
    }
    return result;
#else
    if (path[0] == '/') return path;
    return join_disk_path(current_working_directory(), path);
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
        return lexical_absolute(directory) + "/" + logical_name;
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

bool read_disk_file(const std::string& path, std::string& contents,
                    std::string& error)
{
    if (has_embedded_nul(path)) {
        error = path + ": path contains NUL character";
        return false;
    }
#ifdef _WIN32
    // Wide, long-path-aware open of the UTF-8 spelling: files outside the
    // process code page or below a path longer than MAX_PATH stay
    // addressable, exactly like the reference compiler's io_win32 layer.
    std::vector<wchar_t> wide;
    if (!wide_win32_path(path, wide) || wide.empty()) {
        error = path + ": cannot read file";
        return false;
    }
    nul_terminate(wide);
    HANDLE handle = CreateFileW(&wide[0], GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE |
                                    FILE_SHARE_DELETE,
                                0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        error = read_error_text(path, code, code == ERROR_ACCESS_DENIED);
        return false;
    }
    contents.clear();
    char buffer[8192];
    for (;;) {
        DWORD chunk = 0;
        if (!ReadFile(handle, buffer, sizeof(buffer), &chunk, 0)) {
            const DWORD code = GetLastError();
            error = read_error_text(path, code, code == ERROR_ACCESS_DENIED);
            CloseHandle(handle);
            return false;
        }
        if (chunk == 0) break;
        contents.append(buffer, chunk);
    }
    CloseHandle(handle);
    return true;
#else
    // Clear errno before opening: only the open below may decide that
    // this failure is a denial, never a value some earlier call left
    // behind (a missing file after a denied candidate would otherwise be
    // reported as denied too).
    errno = 0;
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input) {
        error = read_error_text(path, static_cast<unsigned long>(errno),
                                errno == EACCES);
        return false;
    }
    contents.assign(std::istreambuf_iterator<char>(input),
                    std::istreambuf_iterator<char>());
    if (!input.eof() && input.fail()) {
        error = path + ": cannot read file";
        return false;
    }
    return true;
#endif
}

std::string lexical_absolute(const std::string& path)
{
    std::string absolute = resolve_absolute(path);
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
        return current_working_directory();
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
    // A spelling that already carries the prefix is exactly what the object
    // manager expects; adding a second one would address an entry literally
    // named "\\?\...".
    if (has_long_path_prefix(path)) return to_wide_text(path, wide);

    // The prefix bypasses Win32 path parsing, so nothing may be left for
    // that parsing to do: a relative, drive-relative or rooted spelling is
    // resolved to a fully qualified one first, and '.' and '..' are removed
    // lexically because the API no longer interprets them.
    const std::string absolute = lexical_absolute(path);
    if (absolute.empty() || has_long_path_prefix(absolute)) {
        return to_wide_text(absolute.empty() ? path : absolute, wide);
    }

    const bool unc =
        absolute.size() >= 2 && absolute[0] == '/' && absolute[1] == '/';
    const bool drive_absolute =
        absolute.size() >= 3 && is_alpha(absolute[0]) &&
        absolute[1] == ':' && absolute[2] == '/';
    if (!unc && !drive_absolute) {
        // Neither form is prefixable (a bare drive, or a working directory
        // that could not be queried): keep the plain conversion rather
        // than hand Win32 a prefix it would reject.
        return to_wide_text(path, wide);
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
// ---------------------------------------------------------------------------
// Output-side helpers (plan 5 batch 3: the standalone publisher).
//
// Trimmed to the reference compiler's model on 2026-10-07. What is left is the
// directory tree and the write itself, which is what protoc's WriteAllToDisk
// does (command_line_interface.cc:541-610): verify the output directory exists,
// create the missing parents, then open/write/close. The containment, case
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
std::string write_error_text(const std::string& path, unsigned long long code)
{
#ifdef _WIN32
    return path + ": cannot write file (Win32 error " +
           number_text(code) + ")";
#else
    (void)code;
    return path + ": cannot write file";
#endif
}
std::string create_directory_error_text(const std::string& path,
                                        unsigned long long code)
{
#ifdef _WIN32
    return path + ": cannot create directory (Win32 error " +
           number_text(code) + ")";
#else
    (void)code;
    return path + ": cannot create directory";
#endif
}

} // namespace

std::string parent_directory_of(const std::string& path)
{
    std::string trimmed = path;
    while (trimmed.size() > 1 && trimmed[trimmed.size() - 1] == '/') {
        trimmed.resize(trimmed.size() - 1);
    }
    const std::size_t slash = trimmed.rfind('/');
    if (slash == std::string::npos) return std::string();
    if (slash == 0) return "/";
    std::string parent = trimmed.substr(0, slash);
#ifdef _WIN32
    // A bare drive component must stay qualified: "C:" is the root, and
    // "C" would address the drive-relative spelling instead.
    if (parent.size() == 2 && is_alpha(parent[0]) && parent[1] == ':') {
        parent += "/";
    }
#endif
    return parent;
}
bool create_directory_plain(const std::string& path, std::string& error)
{
#ifdef _WIN32
    std::vector<wchar_t> wide;
    if (!wide_win32_path(path, wide) || wide.empty()) {
        error = path + ": cannot create directory";
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
        error = path + ": output path is an existing file, not a directory";
        return false;
    }
    error = create_directory_error_text(path, code);
    return false;
#else
    if (::mkdir(path.c_str(), 0777) == 0) return true;
    if (errno == EEXIST) {
        std::string status_error;
        if (disk_file_status(path, status_error) == FILE_STATUS_DIRECTORY) {
            return true;
        }
        error = path + ": output path is an existing file, not a directory";
        return false;
    }
    const int code = errno;
    error = path + ": cannot create directory (" + std::strerror(code) + ")";
    return false;
#endif
}
bool make_directories_recursive(const std::string& absolute, std::string& error)
{
    if (absolute.empty() || absolute == "/") return true;
#ifdef _WIN32
    if (absolute.size() == 3 && is_alpha(absolute[0]) &&
        absolute[1] == ':' && absolute[2] == '/') {
        return true;
    }
#endif
    if (absolute.size() >= 2 && absolute[0] == '/' && absolute[1] == '/') {
        // A UNC share root is created by its server; only what lies below it
        // is this function's business.
        const std::size_t server_end = absolute.find('/', 2);
        if (server_end == std::string::npos) return true;
        const std::size_t share_end = absolute.find('/', server_end + 1);
        if (share_end == std::string::npos) return true;
    }
    const std::string parent = parent_directory_of(absolute);
    if (!parent.empty() && !make_directories_recursive(parent, error)) {
        return false;
    }
    return create_directory_plain(absolute, error);
}
bool make_directories(const std::string& directory, std::string& error)
{
    error.clear();
    if (directory.empty()) {
        error = "cannot create an empty directory path";
        return false;
    }
    const std::string absolute = lexical_absolute(directory);
    if (absolute.empty()) {
        error = directory + ": cannot resolve directory";
        return false;
    }
    return make_directories_recursive(absolute, error);
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
#ifdef _WIN32
    std::vector<wchar_t> wide;
    if (!wide_win32_path(target, wide) || wide.empty()) {
        error = target + ": cannot write file";
        return false;
    }
    nul_terminate(wide);
    HANDLE handle = CreateFileW(&wide[0], GENERIC_WRITE, 0, 0,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (handle == INVALID_HANDLE_VALUE) {
        error = write_error_text(target, GetLastError());
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
            error = write_error_text(target, code);
            return false;
        }
        done += chunk;
    }
    if (!CloseHandle(handle)) {
        error = write_error_text(target, GetLastError());
        return false;
    }
    return true;
#else
    const int descriptor =
        ::open(target.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (descriptor < 0) {
        const int code = errno;
        error = target + ": cannot write file (" + std::strerror(code) + ")";
        return false;
    }
    std::size_t done = 0;
    while (done < contents.size()) {
        const ssize_t written =
            ::write(descriptor, contents.data() + done, contents.size() - done);
        if (written < 0) {
            const int code = errno;
            ::close(descriptor);
            error = target + ": cannot write file (" + std::strerror(code) + ")";
            return false;
        }
        done += static_cast<std::size_t>(written);
    }
    if (::close(descriptor) != 0) {
        const int code = errno;
        error = target + ": cannot write file (" + std::strerror(code) + ")";
        return false;
    }
    return true;
#endif
}


} // namespace easypb_file
