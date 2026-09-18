#include "file_paths.hpp"

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
#include <string.h>
#include <sys/stat.h>
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
} // namespace

namespace easypb_file {
namespace {

#ifdef _WIN32
bool is_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool is_separator(char c)
{
    return c == '/' || c == '\\';
}

// Defined below: narrow to wide conversion and the single-pass
// decoding map used by the character-aware helpers above them.
bool to_wide_text(const std::string& narrow, std::vector<wchar_t>& wide);
bool to_wide_span(const char* data, std::size_t size,
                  std::vector<wchar_t>& wide);
bool wide_to_utf8(const wchar_t* wide, int wide_length, std::string& utf8);
// Appends the NUL terminator Win32 string APIs require. Conversion
// helpers keep exact sizes (wide.size() stays a unit count); only
// buffers handed to W APIs are terminated, right before the call.
void nul_terminate(std::vector<wchar_t>& wide);
bool decode_text(const std::string& text, std::vector<wchar_t>& wide,
                 std::vector<std::size_t>& ends,
                 std::vector<std::size_t>& units);
std::string normalize_separators(const std::string& path);

typedef BOOL (WINAPI *GetInfoByHandleExFn)(HANDLE, int, LPVOID, DWORD);

DirectoryCaseSensitivityProbe test_directory_case_sensitive_probe = 0;

// Resolves GetFileInformationByHandleEx at runtime so toolchains
// predating the FileCaseSensitiveInfo declaration still build and old
// systems degrade to unknown instead of failing to start. Resolution is
// intentionally repeated per query: it is cheap compared with opening
// the directory and avoids relying on thread-safe function-local static
// initialization, which MSVC did not implement until VS2015.
GetInfoByHandleExFn resolve_case_query_function()
{
    // FARPROC travels as object bytes, never through a cast: a direct
    // function-type cast trips -Wcast-function-type on modern compilers
    // while the void* hop trips -Wpedantic on GCC 4.7, and old GCC
    // cannot silence pedantic with a diagnostic pragma either. With no
    // conversion at all there is nothing to diagnose on any toolchain;
    // the value is used only through the correctly typed pointer below.
    const FARPROC proc = GetProcAddress(
        GetModuleHandleW(L"kernel32.dll"), "GetFileInformationByHandleEx");
    GetInfoByHandleExFn query = 0;
    static_assert(sizeof(query) == sizeof(proc),
                  "function pointers must match for memcpy");
    std::memcpy(&query, &proc, sizeof(query));
    return query;
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
    // Deep checkouts beyond MAX_PATH must not fall back to a wrong
    // directory silently; an empty conversion means exactly that.
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

std::string trim_trailing_separators(const std::string& path){
#ifdef _WIN32
    // Keep a bare drive ("C:") or root ("/", "\\", "C:/") intact.
    // Windows narrow paths are strict UTF-8. ASCII separator bytes cannot
    // occur inside a UTF-8 multibyte sequence, so separator recognition
    // is unambiguous.
    std::vector<wchar_t> wide;
    std::vector<std::size_t> ends, units;
    if (!decode_text(path, wide, ends, units)) {
        // Invalid UTF-8 bytes cannot hold a valid split character;
        // fall back to the byte-wise pass.
        std::size_t end = path.size();
        while (end > 1 && (path[end - 1] == '/' || path[end - 1] == '\\')) {
            if (end == 3 && is_alpha(path[0]) && path[1] == ':' &&
                (path[2] == '/' || path[2] == '\\')) {
                break;
            }
            --end;
        }
        return path.substr(0, end);
    }
    std::size_t end = wide.size();
    while (end > 1 && (wide[end - 1] == L'/' || wide[end - 1] == L'\\')) {
        if (end == 3 && wide[1] == L':' &&
            (wide[2] == L'/' || wide[2] == L'\\') &&
            ((wide[0] >= L'a' && wide[0] <= L'z') ||
             (wide[0] >= L'A' && wide[0] <= L'Z'))) {
            break;
        }
        --end;
    }
    // Map the surviving wide count back to bytes with the same pass
    // data. A straddled multi-unit character cannot occur at a trim
    // cut (cuts land after non-separators or ASCII structure); any
    // mismatch keeps the input verbatim instead of corrupting it.
    std::size_t byte_end = 0;
    {
        std::size_t w = 0;
        for (std::size_t i = 0; i < ends.size() && w < end; ++i) {
            byte_end = ends[i];
            w += units[i];
        }
        if (w != end) return path;
    }
    return path.substr(0, byte_end);
#else
    std::size_t end = path.size();
    while (end > 1 && path[end - 1] == '/') --end;
    return path.substr(0, end);
#endif
}

} // namespace

// Fully qualified paths need no resolution: drive-absolute ("C:/x",
// "C:\x") and UNC ("\\host\..."). Drive-relative ("C:x") and rooted
// ("\x", "/x") forms resolve against per-drive/current directories, so
// they must go through GetFullPathNameW to become comparable for
// search-root prefix matching, and they never qualify for the isolated
// absolute-root convenience.
bool is_fully_qualified_disk_path(const std::string& path)
{
#ifdef _WIN32
    if (path.size() >= 3 && is_alpha(path[0]) && path[1] == ':' &&
        is_separator(path[2])) {
        return true;
    }
    if (path.size() >= 2 && is_separator(path[0]) && is_separator(path[1])) {
        return true;
    }
    return false;
#else
    if (path.empty()) return false;
    return path[0] == '/';
#endif
}

#ifdef _WIN32
bool utf8_from_wide(const wchar_t* text, std::string& utf8)
{
    utf8.clear();
    if (text == 0) return false;
    const int length = lstrlenW(text);
    return wide_to_utf8(text, length, utf8);
}

bool is_directory_case_query_unsupported_error(unsigned long error_code)
{
    return error_code == ERROR_INVALID_PARAMETER ||
           error_code == ERROR_NOT_SUPPORTED ||
           error_code == ERROR_CALL_NOT_IMPLEMENTED;
}

void set_directory_case_sensitivity_probe(DirectoryCaseSensitivityProbe probe)
{
    test_directory_case_sensitive_probe = probe;
}
#endif

int directory_case_sensitive(const std::string& directory)
{
#ifdef _WIN32
    // Numeric FILE_INFO_BY_HANDLE_CLASS value of FileCaseSensitiveInfo
    // and the FILE_CS_FLAG_CASE_SENSITIVE_DIR bit, spelled out so
    // toolchains predating the declarations still build. Verified
    // against the platform headers: the enumerator directly follows
    // FileRenameInfoEx in FILE_INFO_BY_HANDLE_CLASS, and the info
    // struct carries one ULONG flag word.
    enum { CASE_SENSITIVE_INFO_CLASS = 23 };
    enum { CASE_SENSITIVE_DIR_FLAG = 0x00000001 };
    if (test_directory_case_sensitive_probe != 0) {
        return test_directory_case_sensitive_probe(directory);
    }
    const GetInfoByHandleExFn query = resolve_case_query_function();
    if (query == 0) return -1;
    // An unsupported query API means per-directory sensitivity cannot
    // exist on the running system: keep the historical insensitive
    // behavior. Any later failure is reported distinctly (-2) so
    // callers can handle it conservatively instead.
    std::string path = trim_trailing_separators(directory);
    if (path.empty()) return -2;
    if (path.size() == 2 && path[1] == ':' &&
        ((path[0] >= 'a' && path[0] <= 'z') ||
         (path[0] >= 'A' && path[0] <= 'Z'))) {
        path += "/";
    }
    // Metadata rights only: listing (FILE_LIST_DIRECTORY) is not
    // needed and may be denied while traversing the path is allowed.
    // The narrow directory spelling is UTF-8 like every path here.
    std::vector<wchar_t> wide;
    if (!to_wide_text(path, wide) || wide.empty()) return -2;
    nul_terminate(wide);
    HANDLE handle = CreateFileW(
        &wide[0], FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        0, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0);
    if (handle == INVALID_HANDLE_VALUE) return -2;
    DWORD flags = 0;
    const BOOL ok = query(handle, CASE_SENSITIVE_INFO_CLASS, &flags,
                          sizeof(flags));
    const DWORD query_error = ok ? 0 : GetLastError();
    CloseHandle(handle);
    if (!ok) {
        // Capability detection, not just failure reporting: the query
        // symbol exists since Vista while per-directory sensitivity
        // arrived in Windows 10 build 17107, and filesystems or remote
        // providers may not implement the class at all. Those cases
        // mean the capability is unavailable (-1); access, I/O and
        // unexpected errors stay operational failures (-2).
        if (is_directory_case_query_unsupported_error(query_error)) {
            return -1;
        }
        return -2;
    }
    return (flags & CASE_SENSITIVE_DIR_FLAG) != 0 ? 1 : 0;
#else
    (void)directory;
    return -1;
#endif
}

bool is_absolute_disk_path(const std::string& path)
{
    if (path.empty()) return false;
#ifdef _WIN32
    if (path.size() >= 2 && is_alpha(path[0]) && path[1] == ':') return true;
    if (path.size() >= 2 && is_separator(path[0]) && is_separator(path[1])) {
        return true;
    }
    return is_separator(path[0]);
#else
    return path[0] == '/';
#endif
}

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
    // trim keeps root separators ("/", "\\", "C:/") intact; do not add a
    // second one or "/" becomes the UNC path "//...".
    bool base_ends_with_separator = false;
    if (!base.empty()) {
#ifdef _WIN32
        // Character-aware for consistency with the rest of the Windows
        // path layer. In valid UTF-8, ASCII separator bytes cannot be
        // continuation bytes.
        std::vector<wchar_t> wide_base;
        if (to_wide_text(base, wide_base) && !wide_base.empty()) {
            const wchar_t last = wide_base[wide_base.size() - 1];
            base_ends_with_separator = (last == L'/' || last == L'\\');
        } else {
            base_ends_with_separator =
                base[base.size() - 1] == '/' || base[base.size() - 1] == '\\';
        }
#else
        base_ends_with_separator = base[base.size() - 1] == '/';
#endif
    }
    if (base_ends_with_separator) {
        return base + logical_name;
    }
    return base + "/" + logical_name;
}

bool disk_file_exists(const std::string& path)
{
    std::string ignored;
    return disk_file_status(path, ignored) == FILE_STATUS_FILE;
}

namespace {
FileStatusProbe test_status_probe = 0;
FileIdentityProbe test_identity_probe = 0;
} // namespace

void set_file_status_probe(FileStatusProbe probe)
{
    test_status_probe = probe;
}

void set_file_identity_probe(FileIdentityProbe probe)
{
    test_identity_probe = probe;
}

FileStatus disk_file_status(const std::string& path, std::string& error)
{
    error.clear();
    if (test_status_probe != 0) return test_status_probe(path, error);
    if (path.empty()) return FILE_STATUS_ABSENT;
    if (has_embedded_nul(path)) {
        // Native calls would see only the truncated prefix: only ERROR
        // may result, never a match for a different spelling.
        error = path + ": path contains NUL character";
        return FILE_STATUS_ERROR;
    }
#ifdef _WIN32
    std::vector<wchar_t> wide;
    if (!to_wide_text(path, wide) || wide.empty()) {
        // Undecodable input fails loudly like read/identity do: only
        // ABSENT may advance a search to the next root.
        error = path + ": path is not valid UTF-8";
        return FILE_STATUS_ERROR;
    }
    nul_terminate(wide);
    const DWORD attributes = GetFileAttributesW(&wide[0]);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return FILE_STATUS_ABSENT;
        }
        std::ostringstream message;
        message << path << ": cannot access file (Win32 error " << code
                << ")";
        error = message.str();
        return FILE_STATUS_ERROR;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return FILE_STATUS_ABSENT;
    }
    return FILE_STATUS_FILE;
#else
    struct stat status;
    if (::stat(path.c_str(), &status) != 0) {
        const int code = errno;
        if (code == ENOENT || code == ENOTDIR) return FILE_STATUS_ABSENT;
        std::ostringstream message;
        message << path << ": cannot access file: " << ::strerror(code);
        error = message.str();
        return FILE_STATUS_ERROR;
    }
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
    // Wide open of the UTF-8 spelling: files outside the process code
    // page stay addressable.
    std::vector<wchar_t> wide;
    if (!to_wide_text(path, wide) || wide.empty()) {
        error = path + ": cannot read file";
        return false;
    }
    nul_terminate(wide);
    HANDLE handle = CreateFileW(&wide[0], GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE |
                                    FILE_SHARE_DELETE,
                                0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (handle == INVALID_HANDLE_VALUE) {
        std::ostringstream message;
        message << path << ": cannot read file (Win32 error "
                << GetLastError() << ")";
        error = message.str();
        return false;
    }
    contents.clear();
    char buffer[8192];
    for (;;) {
        DWORD chunk = 0;
        if (!ReadFile(handle, buffer, sizeof(buffer), &chunk, 0)) {
            std::ostringstream message;
            message << path << ": cannot read file (Win32 error "
                    << GetLastError() << ")";
            error = message.str();
            CloseHandle(handle);
            return false;
        }
        if (chunk == 0) break;
        contents.append(buffer, chunk);
    }
    CloseHandle(handle);
    return true;
#else
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input) {
        error = path + ": cannot read file";
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

std::string absolute_disk_path(const std::string& path)
{
    if (path.empty()) return path;
#ifdef _WIN32
    // Only fully qualified paths pass through untouched. Drive-relative
    // ("C:x") and rooted ("\x", "/x") forms must be resolved so that
    // equivalent spellings of one file compare equal during logical
    // mapping instead of producing a false duplicate alias. Resolution
    // runs on the wide form so Unicode names are independent of the
    // process ANSI code page; the internal UTF-8 spelling passes through.
    if (is_fully_qualified_disk_path(path)) return path;
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
    if (is_absolute_disk_path(path)) return path;
    return join_disk_path(current_working_directory(), path);
#endif
}

std::string lexical_absolute(const std::string& path)
{
    std::string absolute = absolute_disk_path(path);
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

namespace {

#ifdef _WIN32
// Converts one narrow (UTF-8, the single narrow encoding of this
// module) string to wide characters. Returns false when the spelling
// is not valid UTF-8.
bool to_wide_text(const std::string& narrow, std::vector<wchar_t>& wide)
{
    return to_wide_span(narrow.data(), narrow.size(), wide);
}

// Converts a byte range without allocating a substring: the single
// decoding primitive behind the one-pass walk below.
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

// Uppercases wide text with the locale-invariant table into a separate
// buffer (the required size is queried first: invariant uppercase never
// merges characters, but the size still comes from the API, not from an
// assumption). Returns false when the mapping fails.
bool upper_invariant_copy(const std::vector<wchar_t>& text,
                          std::vector<wchar_t>& upper)
{
    upper.clear();
    if (text.empty()) return true;
    const int need =
        LCMapStringW(LOCALE_INVARIANT, LCMAP_UPPERCASE, &text[0],
                     static_cast<int>(text.size()), 0, 0);
    if (need <= 0) return false;
    upper.resize(static_cast<std::size_t>(need));
    return LCMapStringW(LOCALE_INVARIANT, LCMAP_UPPERCASE, &text[0],
                        static_cast<int>(text.size()), &upper[0],
                        need) > 0;
}

// Decodes the whole string in one pass: the wide form plus, per
// character, its byte end offset and wide-unit count. The walk carries
// its offset forward and converts tiny slices (no rescanning from
// zero, no whole-string substrings), so the total work stays linear in
// the input length. Returns false when the bytes are not valid UTF-8.
bool decode_text(const std::string& text, std::vector<wchar_t>& wide,
                 std::vector<std::size_t>& ends,
                 std::vector<std::size_t>& units)
{
    wide.clear();
    ends.clear();
    units.clear();
    if (!to_wide_text(text, wide)) return false;
    std::vector<wchar_t> unit;
    std::size_t offset = 0;
    std::size_t total = 0;
    while (offset < text.size()) {
        bool advanced = false;
        for (std::size_t len = 1;
             len <= 4 && offset + len <= text.size(); ++len) {
            unit.clear();
            if (!to_wide_span(text.data() + offset, len, unit)) continue;
            // The walk always rests on a character boundary, so the
            // shortest convertible slice is exactly one character: a
            // leading single-byte character already converts at len 1.
            offset += len;
            ends.push_back(offset);
            units.push_back(unit.size());
            total += unit.size();
            advanced = true;
            break;
        }
        if (!advanced) return false;
    }
    // The slice walk must account for every wide unit exactly.
    return total == wide.size();
}

// Rewrites every real backslash as '/'. Windows narrow paths are UTF-8,
// whose continuation bytes can never equal ASCII '\\', so separator
// recognition is unambiguous. Byte ranges come from the single decoding
// pass and the output keeps all non-separator bytes verbatim. Invalid
// UTF-8 is handled byte-wise only so lexical helpers remain total; any
// filesystem or logical-name boundary rejects it before use.
std::string normalize_separators(const std::string& path)
{
    std::vector<wchar_t> wide;
    std::vector<std::size_t> ends, units;
    if (!decode_text(path, wide, ends, units)) {
        std::string result = path;
        for (std::size_t i = 0; i < result.size(); ++i) {
            if (result[i] == '\\') result[i] = '/';
        }
        return result;
    }
    std::string result;
    std::size_t w = 0;
    for (std::size_t i = 0; i < ends.size(); ++i) {
        const std::size_t begin = i == 0 ? 0 : ends[i - 1];
        if (units[i] == 1 && wide[w] == L'\\') {
            result += '/';
        } else {
            result.append(path, begin, ends[i] - begin);
        }
        w += units[i];
    }
    return result;
}

// Case-insensitive wide prefix match that returns the remainder in the
// file's own narrow bytes. The boundary is computed in wide space:
// cutting the narrow text at the root's byte length first would split
// a multibyte character whenever case-equivalent spellings encode with
// different lengths (U+0250 is two bytes in UTF-8, its Windows
// ordinal-uppercase U+2C6F three). The matched region is exactly
// wide_prefix.size() wide characters; folding is verified 1:1 above so
// the boundary cannot split a character. The remainder keeps the
// original bytes verbatim (see decode_text). In
// particular the linguistic NORM_IGNORECASE comparison is deliberately
// avoided: it ignores e.g. U+00AD SOFT HYPHEN placements ("a<AD>b" vs
// "<AD>ab") that name different files, which used to misreport an
// isolated root as shadowed before native identity was even consulted.
// Returns false when the prefix does not match or no byte boundary
// corresponds to the wide match.
bool strip_prefix_wide(const std::string& text, const std::string& prefix,
                       std::string& relative_out)
{
    std::vector<wchar_t> wide_text, wide_prefix;
    std::vector<std::size_t> ends, units;
    std::vector<wchar_t> upper_text, upper_prefix;
    if (!decode_text(text, wide_text, ends, units) ||
        !to_wide_text(prefix, wide_prefix) ||
        !upper_invariant_copy(wide_text, upper_text) ||
        !upper_invariant_copy(wide_prefix, upper_prefix)) {
        return false;
    }
    // Invariant uppercase must neither merge nor expand characters:
    // otherwise the wide boundary would not map back to narrow bytes.
    if (upper_text.size() != wide_text.size() ||
        upper_prefix.size() != wide_prefix.size() ||
        upper_text.size() < upper_prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < upper_prefix.size(); ++i) {
        if (upper_text[i] != upper_prefix[i]) return false;
    }
    // Map the matched wide count back to bytes with the same pass
    // data. A straddled multi-unit character fails instead of
    // splitting the spelling.
    std::size_t byte_offset = 0;
    {
        std::size_t w = 0;
        for (std::size_t i = 0;
             i < ends.size() && w < wide_prefix.size(); ++i) {
            byte_offset = ends[i];
            w += units[i];
        }
        if (w != wide_prefix.size()) return false;
    }
    relative_out = text.substr(byte_offset);
    return true;
}
#endif

// Splits a lexical-absolute path ("C:/a/b", "//host/share/a", "/a/b")
// on '/'. Pure string logic, shared by the Windows fold-aware helpers
// and the cross-platform same-entry confirmation below.
std::vector<std::string> split_path_components(const std::string& path)
{
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= path.size(); ++i) {
        if (i != path.size() && path[i] != '/') continue;
        parts.push_back(path.substr(start, i - start));
        start = i + 1;
    }
    return parts;
}

// Joins the first 'count' components with '/'. Pure string logic,
// shared like split_path_components.
std::string join_path_components(const std::vector<std::string>& parts,
                                 std::size_t count)
{
    std::string result;
    for (std::size_t i = 0; i < count && i < parts.size(); ++i) {
        if (i != 0) result += "/";
        result += parts[i];
    }
    return result;
}

#ifdef _WIN32
// Compares two UTF-8 strings for Windows path purposes: 0 when different,
// 1 when they decode to the exact same UTF-16 text, 2 when they match only
// after locale-invariant uppercase folding (a genuine case difference).
// Conversion or folding failures report different. Callers apply the
// case-sensitivity gate only to state 2: wide-exact names denote one
// entry even in a case-sensitive directory.
int compare_path_text(const std::string& left, const std::string& right)
{
    std::vector<wchar_t> wide_left, wide_right;
    if (!to_wide_text(left, wide_left) ||
        !to_wide_text(right, wide_right)) {
        return 0;
    }
    if (wide_left.size() == wide_right.size()) {
        bool exact = true;
        for (std::size_t i = 0; i < wide_left.size(); ++i) {
            if (wide_left[i] != wide_right[i]) {
                exact = false;
                break;
            }
        }
        if (exact) return 1;
    }
    std::vector<wchar_t> upper_left, upper_right;
    if (!upper_invariant_copy(wide_left, upper_left) ||
        !upper_invariant_copy(wide_right, upper_right)) {
        return 0;
    }
    if (upper_left.size() != wide_left.size() ||
        upper_right.size() != wide_right.size() ||
        upper_left.size() != upper_right.size()) {
        return 0;
    }
    for (std::size_t i = 0; i < upper_left.size(); ++i) {
        if (upper_left[i] != upper_right[i]) return 0;
    }
    return 2;
}

// True when a case-only component difference at 'level' is benign: the
// containing directory (spelled from the file's own components) is not
// case-sensitive. Level 0 (drive letters, UNC empty parts) is always
// benign: the OS path parser treats those case-insensitively.
// Containers that cannot be queried because the API is unavailable keep
// the historical benign behavior (per-directory sensitivity cannot
// exist there). A failed query on a capable system returns false and,
// when operational_error is provided, reports it so the caller can
// diagnose instead of silently treating the path as outside.
bool case_only_component_ok(const std::vector<std::string>& file_parts,
                            std::size_t level, bool* operational_error)
{
    if (level == 0) return true;
    std::string container = join_path_components(file_parts, level);
    if (container.empty() || container == "/") return true;
    if (container.size() == 2 && container[1] == ':' &&
        ((container[0] >= 'a' && container[0] <= 'z') ||
         (container[0] >= 'A' && container[0] <= 'Z'))) {
        // A bare drive ("C:") is not openable as a directory: query the
        // drive root ("C:/") instead, whose sensitivity governs the
        // top-level names like any other containing directory.
        container += "/";
    }
    // A bare server name ("//host") is a namespace, not a directory:
    // network names are case-insensitive by protocol, and the spelling
    // is not openable for a query anyway.
    if (container.size() > 2 && container[0] == '/' && container[1] == '/' &&
        container.find('/', 2) == std::string::npos) {
        return true;
    }
    const int probe = directory_case_sensitive(container);
    if (probe == -2) {
        if (operational_error != 0) *operational_error = true;
        return false;
    }
    return probe != 1;
}
#endif

} // namespace

bool strip_search_root_prefix(const std::string& file_absolute,
                              const std::string& root_absolute,
                              std::string& relative_out,
                              bool* operational_error,
                              bool* used_case_folding)
{
    if (operational_error != 0) *operational_error = false;
    if (used_case_folding != 0) *used_case_folding = false;
    // A root that already ends in '/' needs no added separator.
    std::string prefix = root_absolute;
    if (!prefix.empty() && prefix[prefix.size() - 1] != '/') prefix += "/";
    if (prefix.empty()) {
        relative_out = file_absolute;
        return !relative_out.empty();
    }
    // Byte-identical bytes always name the same directory.
    if (file_absolute.size() >= prefix.size() &&
        file_absolute.compare(0, prefix.size(), prefix) == 0) {
        relative_out = file_absolute.substr(prefix.size());
        return !relative_out.empty();
    }
#ifdef _WIN32
    // Otherwise a case-insensitive match must survive directory case
    // semantics: in a case-sensitive directory "inc" and "INC" are
    // different directories and the file is not under the root.
    std::string candidate;
    if (!strip_prefix_wide(file_absolute, prefix, candidate)) return false;
    const std::vector<std::string> file_parts =
        split_path_components(file_absolute);
    const std::vector<std::string> root_parts = split_path_components(
        prefix.substr(0, prefix.size() - 1));
    if (file_parts.size() <= root_parts.size()) return false;
    for (std::size_t i = 0; i < root_parts.size(); ++i) {
        if (file_parts[i] == root_parts[i]) continue;
        const int relation = compare_path_text(file_parts[i], root_parts[i]);
        if (relation == 0) return false;
        // Any non-identical pair that still matches relied on folding
        // (relation 1 cannot occur for valid UTF-8, whose decoding is
        // injective, but flag it all the same): the caller confirms by
        // native identity, because the invariant table folds characters
        // (e.g. Deseret) that filesystems do not.
        if (used_case_folding != 0) *used_case_folding = true;
        if (relation == 2 &&
            !case_only_component_ok(file_parts, i, operational_error)) {
            return false;
        }
    }
    relative_out = candidate;
    return !relative_out.empty();
#else
    return false;
#endif
}

bool equivalent_disk_spelling(const std::string& left,
                              const std::string& right)
{
    const std::string normal_left = lexical_absolute(left);
    const std::string normal_right = lexical_absolute(right);
    if (normal_left == normal_right) return true;
#ifdef _WIN32
    if (compare_path_text(normal_left, normal_right) == 0) return false;
    // Case-only differences are equivalent only inside case-insensitive
    // directories: hardlink names differing only by case in a sensitive
    // directory are different logical names, not one spelling.
    const std::vector<std::string> left_parts =
        split_path_components(normal_left);
    const std::vector<std::string> right_parts =
        split_path_components(normal_right);
    if (left_parts.size() != right_parts.size()) return false;
    for (std::size_t i = 0; i < left_parts.size(); ++i) {
        if (left_parts[i] == right_parts[i]) continue;
        const int relation = compare_path_text(left_parts[i], right_parts[i]);
        if (relation == 0) return false;
        // The last component is the file name: its container is the
        // parent directory spelled from the left side. A failed query
        // rejects the equivalence (loud alias error downstream) rather
        // than silently merging two names.
        if (relation == 2 && !case_only_component_ok(left_parts, i, 0)) {
            return false;
        }
    }
    return true;
#else
    return false;
#endif
}

namespace {
// True only when nothing exists at the path (confirmed absence):
// files and directories both count as existing. Only then may a
// same-entry check answer "different" quietly — note this is NOT
// disk_file_status, which reports directories as ABSENT because only
// files satisfy a file lookup. Any other failure is operational and
// stays loud.
bool is_absent_path(const std::string& path)
{
    if (has_embedded_nul(path)) return false;
#ifdef _WIN32
    std::vector<wchar_t> wide;
    if (!to_wide_text(path, wide) || wide.empty()) return false;
    nul_terminate(wide);
    const DWORD attributes = GetFileAttributesW(&wide[0]);
    if (attributes != INVALID_FILE_ATTRIBUTES) return false;
    const DWORD code = GetLastError();
    return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
#else
    struct stat status;
    if (::stat(path.c_str(), &status) == 0) return false;
    return errno == ENOENT || errno == ENOTDIR;
#endif
}

// Normalizes one spelled container prefix for an identity query: a
// bare drive ("C:") is not openable and is queried as its root;
// "" stays "" for the caller to skip (never a real divergence).
std::string queryable_prefix(const std::string& prefix)
{
    if (prefix.size() == 2 && prefix[1] == ':' &&
        ((prefix[0] >= 'a' && prefix[0] <= 'z') ||
         (prefix[0] >= 'A' && prefix[0] <= 'Z'))) {
        return prefix + "/";
    }
    return prefix;
}

// True for a bare server namespace ("//host", no share component):
// unopenable, and network names are case-insensitive by protocol.
bool is_bare_server(const std::string& prefix)
{
    return prefix.size() > 2 && prefix[0] == '/' && prefix[1] == '/' &&
           prefix.find('/', 2) == std::string::npos;
}

// Compares the native entry identities of two spelled container
// prefixes: 1 when they denote the same entry, 0 when they are
// confirmed different (including confirmed absence on either side),
// -1 on an operational failure (reported through operational_error).
// Unopenable namespaces stay benign without a query. Prefixes are
// compared entry-wise (no-follow): a trailing junction reports
// itself, so two distinct entries pointing at one target compare
// different even though the target-following file identity matches.
int check_prefix_pair(const std::string& left, const std::string& right,
                      bool* operational_error)
{
    const std::string left_query = queryable_prefix(left);
    const std::string right_query = queryable_prefix(right);
    if (left_query.empty() || right_query.empty() ||
        is_bare_server(left_query) || is_bare_server(right_query)) {
        return 1;
    }
    FileIdentity left_identity, right_identity;
    std::string left_error, right_error;
    const bool left_ok = get_file_identity(left_query, left_identity,
                                           left_error, false);
    const bool right_ok = get_file_identity(right_query, right_identity,
                                            right_error, false);
    if (!left_ok || !right_ok) {
        if ((!left_ok && !is_absent_path(left_query)) ||
            (!right_ok && !is_absent_path(right_query))) {
            if (operational_error != 0) *operational_error = true;
            return -1;
        }
        return 0;
    }
    return same_identity(left_identity, right_identity) ? 1 : 0;
}

// Component relation for same-entry checks: 1 when byte-identical
// (trivially same), 2 when differing only by case folding, 0 when
// genuinely different. POSIX has no folding: anything byte-different
// is different.
int entry_component_relation(const std::string& left,
                             const std::string& right)
{
    if (left == right) return 1;
#ifdef _WIN32
    return compare_path_text(left, right) == 0 ? 0 : 2;
#else
    return 0;
#endif
}
} // namespace

bool confirm_fold_containment(const std::string& root_absolute,
                              const std::string& file_absolute,
                              bool* operational_error)
{
    if (operational_error != 0) *operational_error = false;
    if (root_absolute.empty()) return true;
    // Mirror strip_search_root_prefix: the root without one trailing
    // separator defines the compared depth; the file must be deeper.
    std::string trimmed = root_absolute;
    if (!trimmed.empty() && trimmed[trimmed.size() - 1] == '/') {
        trimmed.erase(trimmed.size() - 1);
    }
    const std::vector<std::string> root_parts =
        split_path_components(trimmed);
    const std::vector<std::string> file_parts =
        split_path_components(file_absolute);
    if (file_parts.size() <= root_parts.size()) return false;
    for (std::size_t i = 0; i < root_parts.size(); ++i) {
        if (file_parts[i] == root_parts[i]) continue;
        if (entry_component_relation(file_parts[i], root_parts[i]) != 2) {
            return false;
        }
        const int check = check_prefix_pair(
            join_path_components(file_parts, i + 1),
            join_path_components(root_parts, i + 1),
            operational_error);
        if (check != 1) return false;
    }
    return true;
}

#ifdef _WIN32
// Exact wide-text compare of a NUL-terminated entry name against an
// unterminated spelling: byte-exact counting must never consult a
// folding table, or it could not separate Deseret entries.
bool wide_text_equal(const wchar_t* nul_text,
                     const std::vector<wchar_t>& text)
{
    std::size_t i = 0;
    while (nul_text[i] != L'\0') {
        if (i >= text.size() || nul_text[i] != text[i]) return false;
        ++i;
    }
    return i == text.size();
}

// Counts exact occurrences of two final-component spellings among the
// entries of a parent directory. Both occurring proves distinct
// entries regardless of any folding table; anything else is one entry
// at most. Enumeration failure is operational, except a confirmed
// absent parent (a race with a known-existing file), which counts as
// zero occurrences.
void count_entry_occurrences(const std::vector<wchar_t>& parent,
                             const std::vector<wchar_t>& first,
                             const std::vector<wchar_t>& second,
                             std::size_t& first_count,
                             std::size_t& second_count,
                             bool& failed)
{
    first_count = 0;
    second_count = 0;
    failed = false;
    std::vector<wchar_t> pattern = parent;
    pattern.push_back(L'\\');
    pattern.push_back(L'*');
    nul_terminate(pattern);
    WIN32_FIND_DATAW found;
    HANDLE handle = FindFirstFileW(&pattern[0], &found);
    if (handle == INVALID_HANDLE_VALUE) {
        failed = true;
        return;
    }
    for (;;) {
        if (wide_text_equal(found.cFileName, first)) ++first_count;
        if (wide_text_equal(found.cFileName, second)) ++second_count;
        if (!FindNextFileW(handle, &found)) {
            if (GetLastError() != ERROR_NO_MORE_FILES) failed = true;
            break;
        }
    }
    FindClose(handle);
}
#endif

bool fold_spellings_same_entry(const std::string& left,
                               const std::string& right,
                               bool* operational_error)
{
    if (operational_error != 0) *operational_error = false;
    // Both spellings must name existing files; the isolated-dedup
    // caller holds confirmed identities for both sides.
    const std::string normal_left = lexical_absolute(left);
    const std::string normal_right = lexical_absolute(right);
    if (normal_left == normal_right) return true;
    const std::vector<std::string> left_parts =
        split_path_components(normal_left);
    const std::vector<std::string> right_parts =
        split_path_components(normal_right);
    if (left_parts.empty() ||
        left_parts.size() != right_parts.size()) {
        return false;
    }
    const std::size_t last = left_parts.size() - 1;
    for (std::size_t i = 0; i < last; ++i) {
        if (left_parts[i] == right_parts[i]) continue;
        if (entry_component_relation(left_parts[i], right_parts[i]) != 2) {
            return false;
        }
        const int check = check_prefix_pair(
            join_path_components(left_parts, i + 1),
            join_path_components(right_parts, i + 1),
            operational_error);
        if (check != 1) return false;
    }
    if (left_parts[last] == right_parts[last]) return true;
    if (entry_component_relation(left_parts[last],
                                 right_parts[last]) != 2) {
        return false;
    }
#ifdef _WIN32
    std::vector<wchar_t> wide_first, wide_second, wide_parent;
    if (!to_wide_text(left_parts[last], wide_first) ||
        !to_wide_text(right_parts[last], wide_second) ||
        !to_wide_text(join_path_components(left_parts, last),
                      wide_parent) ||
        wide_parent.empty()) {
        if (operational_error != 0) *operational_error = true;
        return false;
    }
    std::size_t first_count = 0, second_count = 0;
    bool failed = false;
    count_entry_occurrences(wide_parent, wide_first, wide_second,
                            first_count, second_count, failed);
    if (failed) {
        if (!is_absent_path(join_path_components(left_parts, last))) {
            if (operational_error != 0) *operational_error = true;
        }
        return false;
    }
    // Both spellings occurring as exact entries proves distinct
    // entries with no dependence on any folding table. Anything else
    // is one entry at most: either exactly one spelling occurs (the
    // other addresses it through folding), or neither occurs exactly
    // (a single entry addressed purely through folding, e.g. an entry
    // spelled a third way that both spellings fold to). The latter
    // must still deduplicate: rejecting it would turn one file under
    // two equivalent spellings into a false duplicate error. A race
    // that deletes the entry mid-check fails loudly downstream when
    // the file is actually read.
    return !(first_count > 0 && second_count > 0);
#else
    return false;
#endif
}

bool get_file_identity(const std::string& path, FileIdentity& identity,
                       std::string& error, bool follow_final_reparse)
{
    identity = FileIdentity();
    if (has_embedded_nul(path)) {
        error = path + ": path contains NUL character";
        return false;
    }
    // Deterministic failures sort before the filesystem: tainted input
    // above must fail regardless of any installed probe.
    if (test_identity_probe != 0) {
        return test_identity_probe(path, identity, error);
    }
#ifdef _WIN32
    std::vector<wchar_t> wide;
    if (!to_wide_text(path, wide) || wide.empty()) {
        error = path + ": cannot open file for identity";
        return false;
    }
    nul_terminate(wide);
    // Metadata rights only: identity needs GetFileInformationByHandle,
    // not data reads. A file that is stat-able but locked or ACL'd
    // against data reads must still resolve instead of aborting the
    // load; this matches directory_case_sensitive() below.
    // Without follow_final_reparse the open additionally carries
    // OPEN_REPARSE_POINT, so a trailing junction/symlink reports its
    // own entry instead of its target; backup semantics already covers
    // directories in both modes.
    const DWORD create_flags = follow_final_reparse
        ? static_cast<DWORD>(FILE_ATTRIBUTE_NORMAL)
        : static_cast<DWORD>(FILE_FLAG_BACKUP_SEMANTICS |
                             FILE_FLAG_OPEN_REPARSE_POINT);
    HANDLE handle = CreateFileW(&wide[0], FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                0, OPEN_EXISTING, create_flags, 0);
    if (handle == INVALID_HANDLE_VALUE && follow_final_reparse) {
        // Missing paths must not pay for a second doomed open:
        // 0xFFFFFFFF has the directory bit set, so check validity
        // first and retry with backup semantics for real directories
        // only. Identity works for any filesystem object: prefix-entry
        // checks rely on directory identities, while every other
        // caller passes files, so file behavior is unchanged.
        const DWORD attributes = GetFileAttributesW(&wide[0]);
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            handle = CreateFileW(&wide[0], FILE_READ_ATTRIBUTES,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 0, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0);
        }
    }
    if (handle == INVALID_HANDLE_VALUE) {
        std::ostringstream message;
        message << path << ": cannot open file for identity (Win32 error "
                << GetLastError() << ")";
        error = message.str();
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info;
    const BOOL ok = GetFileInformationByHandle(handle, &info);
    // Capture before CloseHandle, which may reset the code.
    const DWORD query_error = ok ? 0 : GetLastError();
    CloseHandle(handle);
    if (!ok) {
        std::ostringstream message;
        message << path << ": cannot query file identity (Win32 error "
                << query_error << ")";
        error = message.str();
        return false;
    }
    identity.valid = true;
    // The (volume, file-index) pair identifies the file: the 64-bit file
    // index (high/low) is unique within its volume, and the volume serial
    // tells volumes apart.
    identity.key_a = static_cast<std::uint64_t>(info.dwVolumeSerialNumber);
    identity.key_b = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) |
                     static_cast<std::uint64_t>(info.nFileIndexLow);
    return true;
#else
    struct stat status;
    // lstat reports a trailing symlink itself; stat follows it to the
    // target. Only the no-follow form is used, and only by entry
    // confirmation, which never fires on POSIX (exact matching).
    const int status_rc = follow_final_reparse
        ? ::stat(path.c_str(), &status)
        : ::lstat(path.c_str(), &status);
    if (status_rc != 0) {
        std::ostringstream message;
        message << path << ": cannot query file identity: "
                << ::strerror(errno);
        error = message.str();
        return false;
    }
    identity.valid = true;
    identity.key_a = static_cast<std::uint64_t>(status.st_dev);
    identity.key_b = static_cast<std::uint64_t>(status.st_ino);
    return true;
#endif
}

bool same_identity(const FileIdentity& left, const FileIdentity& right)
{
    return left.valid && right.valid && left.key_a == right.key_a &&
           left.key_b == right.key_b;
}

bool identity_less(const FileIdentity& left, const FileIdentity& right)
{
    // Order by validity first so an invalid identity (failed query, never
    // stored) can never compare equal to a valid cached one. The loader
    // only inserts identities from successful queries; this keeps a
    // hypothetical invalid lookup from aliasing a real file.
    if (left.valid != right.valid) return left.valid < right.valid;
    if (left.key_a != right.key_a) return left.key_a < right.key_a;
    return left.key_b < right.key_b;
}

} // namespace easypb_file
