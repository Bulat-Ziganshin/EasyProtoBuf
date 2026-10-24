// Real filesystem discovery and caching tests for the source loader
// (codegen/proto_loader.hpp/.cpp plus file_paths). Full builds only.
// Permanent fixtures live under tests/codegen/imports/data/loader/ and
// remain unchanged; files created here go under a work directory below
// the CTest working directory pinned in tests/CMakeLists.txt (the CMake
// tests binary directory) and are rewritten on every run so repeated
// runs never depend on stale data.
//
// The suite pins the contract of the loader against the reference
// compiler (protoc): caller-ordered roots with the first match winning,
// disk-path arguments mapped through a byte-wise (case-sensitive) root
// prefix, shadowed inputs rejected, missing and cyclic imports reported,
// one parse per logical name, and no identity or case folding anywhere.
// A few tests deliberately document where this loader is stricter than
// protoc (its own diagnostic codes and positions, and import spellings
// protoc would treat as a missing file).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "file_paths.hpp"
#include "logical_paths.hpp"
#include "proto_loader.hpp"
#include "schema.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#else
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

int failures = 0;
int skips = 0;

void check(bool condition, const char* expression, const char* file, int line)
{
    if (!condition) {
        std::cerr << file << ':' << line << ": CHECK failed: " << expression << '\n';
        ++failures;
    }
}

#define CHECK(x) check((x), #x, __FILE__, __LINE__)

#define SKIP(reason)                                                    \
    do {                                                                \
        std::cout << "SKIP " << reason << '\n';                         \
        ++skips;                                                        \
        return;                                                         \
    } while (0)

std::string text(const str_view& value)
{
    return std::string(value.data(), value.size());
}

#ifdef _WIN32
// Converts a UTF-8 test spelling for a Win32 W call. Windows loader paths
// use the same UTF-8 representation and never depend on the process ANSI
// code page.
bool wide_from_utf8(const std::string& text, std::vector<wchar_t>& wide);

// Grants Everyone full control and leaves the DACL unprotected again, so
// a file whose access was denied below can be rewritten and deleted.
bool allow_full_access(const std::string& path);

// Adds a deny ACE for Everyone covering GENERIC_READ and protects the
// DACL, which makes a status query fail with ERROR_ACCESS_DENIED. This is
// the Windows way to reproduce what EACCES means on POSIX: the file
// exists and is not reachable, which is not the same as absence.
bool deny_read_access(const std::string& path);
#endif

// Creates 'link' as an alias of existing file 'target' (defined below).
bool make_link(const std::string& target, const std::string& link);

std::string fixture_dir;
std::string work_root = "./imports-loader-work";

std::string join_arg(const std::string& dir, const std::string& leaf)
{
    if (dir.empty()) return leaf;
    std::string trimmed = dir;
    while (!trimmed.empty() &&
           (trimmed[trimmed.size() - 1] == '/' ||
            trimmed[trimmed.size() - 1] == '\\')) {
        trimmed.erase(trimmed.size() - 1);
    }
    return trimmed + "/" + leaf;
}

bool make_one_dir(const std::string& path)
{
#ifdef _WIN32
    std::vector<wchar_t> wide;
    if (!wide_from_utf8(path, wide)) return false;
    if (CreateDirectoryW(&wide[0], 0)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
#else
    if (::mkdir(path.c_str(), 0755) == 0) return true;
    return errno == EEXIST;
#endif
}

bool make_dirs(const std::string& path)
{
    std::string normalized = path;
#ifdef _WIN32
    for (std::size_t i = 0; i < normalized.size(); ++i) {
        if (normalized[i] == '\\') normalized[i] = '/';
    }
#endif
    // Preserve an absolute prefix while creating parts incrementally.
    std::size_t start = 0;
    std::string prefix;
    std::string current;
#ifdef _WIN32
    if (normalized.size() >= 2 && ((normalized[0] >= 'a' && normalized[0] <= 'z') ||
                                  (normalized[0] >= 'A' && normalized[0] <= 'Z')) &&
        normalized[1] == ':') {
        prefix = normalized.substr(0, 2);
        current = prefix;
        start = 2;
        if (start < normalized.size() && normalized[start] == '/') {
            current += "/";
            ++start;
        }
    } else if (!normalized.empty() && normalized[0] == '/') {
        prefix = "/";
        current = "/";
        start = 1;
    }
#else
    if (!normalized.empty() && normalized[0] == '/') {
        prefix = "/";
        current = "/";
        start = 1;
    }
#endif
    for (std::size_t i = start; i <= normalized.size(); ++i) {
        if (i != normalized.size() && normalized[i] != '/') continue;
        const std::string part = normalized.substr(start, i - start);
        start = i + 1;
        if (part.empty() || part == ".") continue;
        std::string next;
        if (current.empty()) next = part;
        else if (current[current.size() - 1] == '/') next = current + part;
        else next = current + "/" + part;
        current = next;
        if (!make_one_dir(current)) return false;
    }
    return true;
}

bool write_file(const std::string& path, const std::string& contents)
{
    const std::size_t slash = path.find_last_of("/\\");
    if (slash != std::string::npos) {
        if (!make_dirs(path.substr(0, slash))) return false;
    }
#ifdef _WIN32
    std::vector<wchar_t> wide;
    if (!wide_from_utf8(path, wide)) return false;
    HANDLE handle = CreateFileW(&wide[0], GENERIC_WRITE, 0, 0,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (handle == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok =
        WriteFile(handle, contents.data(),
                  static_cast<DWORD>(contents.size()), &written, 0) &&
        written == static_cast<DWORD>(contents.size());
    CloseHandle(handle);
    return ok != FALSE;
#else
    std::ofstream output(path.c_str(), std::ios::out | std::ios::binary);
    if (!output) return false;
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.close();
    return !!output;
#endif
}

void require_work_root()
{
    if (!make_dirs(work_root)) {
        std::cerr << "cannot create work directory " << work_root << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::string work_path(const std::string& relative)
{
    return join_arg(work_root, relative);
}

#ifdef _WIN32
// Converts a UTF-8 test spelling for a Win32 W call (defined above).
bool wide_from_utf8(const std::string& text, std::vector<wchar_t>& wide)
{
    wide.clear();
    if (text.empty()) return true;
    const int need =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), 0, 0);
    if (need <= 0) return false;
    wide.resize(static_cast<std::size_t>(need));
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), &wide[0],
                            need) != need) {
        return false;
    }
    wide.push_back(L'\0');
    return true;
}
#endif

#ifdef _WIN32
// Builds one explicit access entry for Everyone and applies it to path.
// A failure means the filesystem cannot store the DACL (FAT/exFAT, some
// network shares), which the caller reports as a skip rather than as a
// loader failure.
bool apply_everyone_entry(const std::string& path, ACCESS_MODE access_mode,
                          DWORD permissions, DWORD inheritance,
                          SECURITY_INFORMATION security_information)
{
    std::vector<wchar_t> wide;
    if (!wide_from_utf8(path, wide)) return false;

    BYTE everyone[SECURITY_MAX_SID_SIZE];
    DWORD everyone_size = sizeof(everyone);
    if (!CreateWellKnownSid(WinWorldSid, 0, everyone, &everyone_size)) {
        return false;
    }

    EXPLICIT_ACCESS_W entry;
    std::memset(&entry, 0, sizeof(entry));
    entry.grfAccessPermissions = permissions;
    entry.grfAccessMode = access_mode;
    entry.grfInheritance = inheritance;
    entry.Trustee.pMultipleTrustee = 0;
    entry.Trustee.MultipleTrusteeOperation = NO_MULTIPLE_TRUSTEE;
    entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    entry.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    entry.Trustee.ptstrName = reinterpret_cast<LPWSTR>(everyone);

    PACL acl = 0;
    if (SetEntriesInAclW(1, &entry, 0, &acl) != ERROR_SUCCESS) return false;
    const DWORD result =
        SetNamedSecurityInfoW(&wide[0], SE_FILE_OBJECT,
                              security_information, 0, 0, acl, 0);
    LocalFree(acl);
    return result == ERROR_SUCCESS;
}

bool allow_full_access(const std::string& path)
{
    return apply_everyone_entry(path, GRANT_ACCESS, GENERIC_ALL,
                                SUB_CONTAINERS_AND_OBJECTS_INHERIT,
                                DACL_SECURITY_INFORMATION |
                                    UNPROTECTED_DACL_SECURITY_INFORMATION);
}

bool deny_read_access(const std::string& path)
{
    // PROTECTED drops inherited allowances, so the deny ACE is the whole
    // DACL. Ownership still grants WRITE_DAC, which is what allows the
    // cleanup below to restore access.
    return apply_everyone_entry(path, DENY_ACCESS, GENERIC_READ,
                                NO_INHERITANCE,
                                DACL_SECURITY_INFORMATION |
                                    PROTECTED_DACL_SECURITY_INFORMATION);
}

void remove_dir_if_present(const std::string& path)
{
    std::vector<wchar_t> wide;
    if (!wide_from_utf8(path, wide)) return;
    RemoveDirectoryW(&wide[0]);
}
#endif

bool path_is_file(const std::string& path)
{
    std::string ignored;
    return easypb_file::disk_file_status(path, ignored) ==
           easypb_file::FILE_STATUS_FILE;
}

// Deletes a file whose absence is a test precondition: a stale file left
// by an interrupted run must not flip an expected failure into a success.
void remove_if_present(const std::string& path)
{
#ifdef _WIN32
    std::vector<wchar_t> wide;
    if (!wide_from_utf8(path, wide)) return;
    DeleteFileW(&wide[0]);
#else
    std::remove(path.c_str());
#endif
}

// Encodes one BMP code point as UTF-8 for a path spelling. The loader
// converts that spelling only at the W-API boundary.
std::string utf8_from_code_point(unsigned int code)
{
    std::string result;
    if (code < 0x80u) {
        result += static_cast<char>(code);
    } else if (code < 0x800u) {
        result += static_cast<char>(0xC0u | (code >> 6));
        result += static_cast<char>(0x80u | (code & 0x3Fu));
    } else if (code < 0x10000u) {
        result += static_cast<char>(0xE0u | (code >> 12));
        result += static_cast<char>(0x80u | ((code >> 6) & 0x3Fu));
        result += static_cast<char>(0x80u | (code & 0x3Fu));
    }
    return result;
}

void test_diamond_caching()
{
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(join_arg(fixture_dir, "diamond"));
    std::vector<std::string> roots(1, "root.proto");
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    CHECK(easypb_proto::load_source_files(options, roots, files, error));
    if (files.files().size() != 4) {
        CHECK(files.files().size() == 4);
        return;
    }
    CHECK(files.targets().size() == 1);
    // Each unique logical file was read once: shared.proto is reached
    // through both left and right but parsed a single time.
    CHECK(files.find_file("root.proto") != 0);
    CHECK(files.find_file("left.proto") != 0);
    CHECK(files.find_file("right.proto") != 0);
    CHECK(files.find_file("shared.proto") != 0);
    // Deferred parsing leaves named types unresolved with raw spellings;
    // the loader must not perform type linking.
    easypb_schema::SchemaFile* left = files.find_file("left.proto");
    CHECK(left != 0);
    if (left != 0 && !left->file.message_type.empty() &&
        !left->file.message_type[0].field.empty()) {
        const FieldDescriptorProto& field = left->file.message_type[0].field[0];
        CHECK(field.has_type_name);
        CHECK(!field.has_type);
        CHECK(text(field.type_name) == "Shared");
    }
    // Physical paths are retained while logical names drive the graph.
    easypb_schema::SchemaFile* shared = files.find_file("shared.proto");
    CHECK(shared != 0);
    if (shared != 0) CHECK(!shared->physical_name.empty());
    // Later reads and metadata stay valid after pool and vector growth.
    const std::string before = text(shared->file.package);
    for (int i = 0; i < 100; ++i) {
        char name[64];
#if defined(_MSC_VER)
        _snprintf(name, sizeof(name), "growth-%d.proto", i);
#else
        std::snprintf(name, sizeof(name), "growth-%d.proto", i);
#endif
        try {
            files.add_file(name);
        } catch (...) {
            CHECK(false);
            return;
        }
    }
    CHECK(text(shared->file.package) == before);
    CHECK(text(shared->file.message_type[0].name) == "Shared");
    // Grow the pool of an already loaded file past its first block, then
    // verify its retained metadata and bound edges still read correctly:
    // pool blocks never move, so earlier views stay valid.
    if (shared != 0) {
        for (int i = 0; i < 2000; ++i) {
            shared->strings.save("padding text to force more blocks");
        }
        CHECK(text(shared->file.package) == before);
        CHECK(text(shared->file.message_type[0].name) == "Shared");
        CHECK(text(shared->file.message_type[0].field[0].name) == "value");
        easypb_schema::SchemaFile* root = files.find_file("root.proto");
        CHECK(root != 0);
        if (root != 0) CHECK(root->edges.size() == 2);
    }
}

void test_search_precedence_first_wins()
{
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(join_arg(fixture_dir, "search/root-a"));
    options.proto_paths.push_back(join_arg(fixture_dir, "search/root-b"));
    std::vector<std::string> roots(1, "app.proto");
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    CHECK(easypb_proto::load_source_files(options, roots, files, error));
    easypb_schema::SchemaFile* shared = files.find_file("shared.proto");
    CHECK(shared != 0);
    if (shared != 0) CHECK(text(shared->file.package) == "first");
}

#ifndef _WIN32
// Whether a denied directory is actually denied for this process.
bool denial_effective(const std::string& denied_dir,
                      const std::string& file_inside)
{
    if (::chmod(denied_dir.c_str(), 0) != 0) return false;
    std::string status_error;
    const easypb_file::FileStatus status =
        easypb_file::disk_file_status(file_inside, status_error);
    const bool denied = status == easypb_file::FILE_STATUS_ERROR &&
                        status_error.find("denied") != std::string::npos;
    if (!denied) ::chmod(denied_dir.c_str(), 0755);
    return denied;
}

// A denied access stops the search: only confirmed absence may advance
// to the next root. The reference compiler draws the line in the same
// place (it treats a denied access as fatal and every other failure as
// absence), so a first-root denial must never silently select the file
// of a later root.
void test_denied_access_stops_search()
{
    require_work_root();
    const std::string first = work_path("denied-first");
    const std::string second = work_path("denied-second");
    CHECK(write_file(join_arg(first, "shared.proto"),
                     "syntax = \"proto2\";\nmessage First {}\n"));
    CHECK(write_file(join_arg(second, "shared.proto"),
                     "syntax = \"proto2\";\nmessage Second {}\n"));
    if (!denial_effective(first, join_arg(first, "shared.proto"))) {
        SKIP("denied access: the process reads through a denied directory");
    }

    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(first);
    options.proto_paths.push_back(second);
    std::vector<std::string> roots(1, "shared.proto");
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    const bool ok =
        easypb_proto::load_source_files(options, roots, files, error);
    ::chmod(first.c_str(), 0755);

    CHECK(!ok);
    CHECK(error.message.find("read access is denied") != std::string::npos);
    CHECK(files.files().empty());
    CHECK(files.targets().empty());

    // With the denial lifted the same search takes the first root.
    easypb_schema::SchemaSet allowed;
    CHECK(easypb_proto::load_source_files(options, roots, allowed, error));
    easypb_schema::SchemaFile* shared = allowed.find_file("shared.proto");
    CHECK(shared != 0);
    if (shared != 0 && !shared->file.message_type.empty()) {
        CHECK(text(shared->file.message_type[0].name) == "First");
    }
}

// The denial of a nested import keeps its import context: the failing
// file, the declaration site and the discovery chain from the selected
// root all survive into the diagnostic.
void test_nested_denied_import_preserves_context()
{
    require_work_root();
    const std::string dir = work_path("nested-denied");
    const std::string blocked = join_arg(dir, "blocked");
    CHECK(write_file(join_arg(dir, "root.proto"),
                     "syntax = \"proto2\";\n"
                     "import \"middle.proto\";\n"
                     "message Root {}\n"));
    CHECK(write_file(join_arg(dir, "middle.proto"),
                     "syntax = \"proto2\";\n"
                     "import \"blocked/leaf.proto\";\n"
                     "message Middle {}\n"));
    CHECK(write_file(join_arg(blocked, "leaf.proto"),
                     "syntax = \"proto2\";\nmessage Leaf {}\n"));
    if (!denial_effective(blocked, join_arg(blocked, "leaf.proto"))) {
        SKIP("nested denied access: the process reads through a denied directory");
    }

    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(dir);
    std::vector<std::string> roots(1, "root.proto");
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    const bool ok =
        easypb_proto::load_source_files(options, roots, files, error);
    ::chmod(blocked.c_str(), 0755);

    CHECK(!ok);
    CHECK(error.file == "middle.proto");
    CHECK(error.location.line == 2);
    CHECK(error.message.find("cannot resolve import \"blocked/leaf.proto\"") !=
          std::string::npos);
    CHECK(error.message.find("read access is denied") != std::string::npos);
    CHECK(error.message.find("searched") != std::string::npos);
    CHECK(error.message.find(
        "\"root.proto\" -> \"middle.proto\" -> \"blocked/leaf.proto\"") !=
        std::string::npos);
    CHECK(files.files().empty());
    CHECK(files.targets().empty());
}
#endif

void test_nul_byte_in_disk_path_fails_loudly()
{
    require_work_root();
    const std::string dir = work_path("nul-path");
    CHECK(write_file(join_arg(dir, "lonely.proto"),
                     "syntax = \"proto2\";\nmessage Lonely {}\n"));

    // The file exists, but the requested spelling carries a NUL suffix
    // that native calls would truncate: the loader must fail loudly
    // instead of reporting a successful load under a confused name.
    const std::string tainted =
        easypb_file::lexical_absolute(join_arg(dir, "lonely.proto")) +
        std::string("\0ignored-suffix", 15);
    std::string status_error;
    CHECK(easypb_file::disk_file_status(tainted, status_error) ==
          easypb_file::FILE_STATUS_ERROR);
    CHECK(status_error.find("NUL") != std::string::npos);

    easypb_proto::SourceLoaderOptions options;
    std::vector<std::string> roots(1, tainted);
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    const bool ok =
        easypb_proto::load_source_files(options, roots, files, error);

    CHECK(!ok);
    CHECK(error.message.find("NUL") != std::string::npos);
    CHECK(files.files().empty());
    CHECK(files.targets().empty());
}

void test_nul_byte_in_import_fails()
{
    require_work_root();
    const std::string dir = work_path("nul-import");
    // A real leaf.proto exists so that only the NUL handling decides
    // the outcome. Import spellings are validated before any disk
    // probe, and NUL is a control character, so this fails at spelling
    // validation; the disk-level NUL guard (previous test) covers
    // spellings that reach the filesystem first, such as CLI roots.
    // The NUL reaches the loader through a `\0` octal escape (a raw
    // NUL byte in source already fails loudly in the parser as an
    // unterminated literal).
    CHECK(write_file(join_arg(dir, "root.proto"),
                     "syntax = \"proto2\";\n"
                     "import \"leaf\\0tail.proto\";\n"
                     "message Root {}\n"));
    CHECK(write_file(join_arg(dir, "leaf.proto"),
                     "syntax = \"proto2\";\nmessage Leaf {}\n"));

    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(dir);
    std::vector<std::string> roots(1, "root.proto");
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    const bool ok =
        easypb_proto::load_source_files(options, roots, files, error);

    CHECK(!ok);
    CHECK(error.message.find("control characters") != std::string::npos);
    CHECK(files.files().empty());
    CHECK(files.targets().empty());
}

void test_shadowed_root_reports_error()
{
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(join_arg(fixture_dir, "search/root-a"));
    options.proto_paths.push_back(join_arg(fixture_dir, "search/root-b"));
    std::vector<std::string> roots;
    roots.push_back("app.proto");
    // Explicitly naming the shadowed file must not silently rename it or
    // switch the cached import to the second package. The reference
    // compiler reports the same state for a shadowed disk-path input.
    roots.push_back(join_arg(join_arg(fixture_dir, "search/root-b"),
                             "shared.proto"));
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    CHECK(!easypb_proto::load_source_files(options, roots, files, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_IMPORT);
    CHECK(error.message.find("shadow") != std::string::npos);
    CHECK(files.files().empty());
    CHECK(files.targets().empty());
}

void test_disk_root_with_invalid_logical_name_fails()
{
    // A disk-path root physically under a search root must map to a
    // valid logical name: quotes are forbidden in logical names, so the
    // remainder below is invalid and the root must fail.
    require_work_root();
    const std::string dir = work_path("invalid-logical-root");
    const std::string leaf = "bad'name.proto";
    CHECK(write_file(join_arg(dir, leaf),
                     "syntax = \"proto2\";\nmessage Bad {}\n"));
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(dir);
    std::vector<std::string> roots(1, join_arg(dir, leaf));
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    CHECK(!easypb_proto::load_source_files(options, roots, files, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_IMPORT);
    CHECK(error.message.find("invalid logical name") != std::string::npos);
    CHECK(files.files().empty());
    CHECK(files.targets().empty());

    // A narrower later root that yields a valid name still wins: the
    // invalid remainder under the wider root is remembered, not fatal,
    // while the search continues.
    const std::string quoted = join_arg(dir, "quoted'dir");
    CHECK(write_file(join_arg(quoted, "ok.proto"),
                     "syntax = \"proto2\";\nmessage Ok {}\n"));
    easypb_proto::SourceLoaderOptions narrowed;
    narrowed.proto_paths.push_back(dir);
    narrowed.proto_paths.push_back(quoted);
    std::vector<std::string> ok_roots(1, join_arg(quoted, "ok.proto"));
    easypb_schema::SchemaSet ok_files;
    CHECK(easypb_proto::load_source_files(narrowed, ok_roots, ok_files,
                                          error));
    CHECK(ok_files.files().size() == 1);
    CHECK(ok_files.targets().size() == 1);
}

void test_directory_argument_is_reported_as_directory()
{
    // protoc maps a directory argument like any other path and then
    // fails to open it ("Input file is a directory"), while a directory
    // under no search root gets the ordinary outside-roots failure.
    // Neither case may surface as a plain "not found".
    require_work_root();
    const std::string dir = work_path("directory-argument");
    CHECK(make_dirs(dir));

    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(work_root);
    std::vector<std::string> roots(1, dir);
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    CHECK(!easypb_proto::load_source_files(options, roots, files, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_IMPORT);
    CHECK(error.message.find("is a directory") != std::string::npos);
    CHECK(files.files().empty());
    CHECK(files.targets().empty());

    // Outside every search root the distinction disappears: this is the
    // same failure as a file that lies outside all roots.
    easypb_proto::SourceLoaderOptions elsewhere;
    elsewhere.proto_paths.push_back(join_arg(fixture_dir, "diamond"));
    easypb_schema::SchemaSet outside_files;
    CHECK(!easypb_proto::load_source_files(elsewhere, roots, outside_files,
                                          error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_IMPORT);
    CHECK(error.message.find("outside all search roots") !=
          std::string::npos);
    CHECK(outside_files.files().empty());

    // A directory is not a search candidate either: a logical name that
    // exists only as a directory advances the search and ends as a
    // missing import, exactly like an absent file.
    std::vector<std::string> logical_roots(1, "directory-argument");
    easypb_schema::SchemaSet logical_files;
    CHECK(!easypb_proto::load_source_files(options, logical_roots,
                                          logical_files, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_MISSING_IMPORT);
    CHECK(error.message.find("not found") != std::string::npos);
    CHECK(logical_files.files().empty());
}

void test_empty_input_argument_is_rejected()
{
    // An empty argument is invalid input, not a spelling to interpret:
    // protoc refuses one outright, and the loader must fail with a
    // message that names the problem. It must not be mistaken for an
    // empty logical path, and it must not leave a partial result behind.
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(join_arg(fixture_dir, "diamond"));
    std::vector<std::string> empty_only(1, std::string());
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    CHECK(!easypb_proto::load_source_files(options, empty_only, files, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_IMPORT);
    CHECK(error.message.find("is empty") != std::string::npos);
    CHECK(error.message.find("empty import path") == std::string::npos);
    CHECK(files.files().empty());
    CHECK(files.targets().empty());

    // A valid argument before it does not make the empty one acceptable,
    // and the failed load still discards the files it had already read.
    std::vector<std::string> mixed;
    mixed.push_back("root.proto");
    mixed.push_back(std::string());
    easypb_schema::SchemaSet mixed_files;
    CHECK(!easypb_proto::load_source_files(options, mixed, mixed_files,
                                          error));
    CHECK(error.message.find("is empty") != std::string::npos);
    CHECK(mixed_files.files().empty());
    CHECK(mixed_files.targets().empty());

    // The contrast: an empty *search root entry* stays meaningful and
    // means the working directory, so the search simply advances to the
    // next root. Only the empty input argument is invalid.
    easypb_proto::SourceLoaderOptions with_empty_root;
    with_empty_root.proto_paths.push_back(std::string());
    with_empty_root.proto_paths.push_back(join_arg(fixture_dir, "diamond"));
    std::vector<std::string> one_root(1, "root.proto");
    easypb_schema::SchemaSet cwd_files;
    easypb_schema::Diagnostic cwd_error;
    CHECK(easypb_proto::load_source_files(with_empty_root, one_root, cwd_files,
                                         cwd_error));
    CHECK(cwd_files.files().size() == 4);
    CHECK(cwd_files.targets().size() == 1);
}

#ifdef _WIN32
void test_denied_access_stops_search()
{
    // Windows counterpart of the POSIX test with the same name. EACCES is
    // reproduced with a deny ACE for Everyone, because the rule has to
    // hold on the platform this loader is developed on.
    //
    // The layer that notices differs from POSIX: GetFileAttributesW
    // answers from the directory entry without evaluating the file's
    // DACL, so the status query still reports a present file and the
    // denial surfaces when the file is read (protoc sees it earlier only
    // because its search opens every candidate). The rule under test is
    // the same either way: a denial fails the load loudly and never falls
    // back to another root's file with the same logical name, even though
    // a healthy candidate exists there.
    require_work_root();
    std::ostringstream unique;
    unique << "denied-access-"
           << static_cast<unsigned long>(GetCurrentProcessId());
    const std::string denied_root = work_path(unique.str());
    const std::string healthy_root = work_path("denied-access-fallback");
    const std::string denied_file = join_arg(denied_root, "shared.proto");
    const std::string fallback_file =
        join_arg(healthy_root, "shared.proto");
    const std::string importer_file =
        join_arg(denied_root, "importer.proto");
    CHECK(make_dirs(denied_root));
    CHECK(make_dirs(healthy_root));
    const std::string body = "syntax = \"proto2\";\nmessage Shared {}\n";
    CHECK(write_file(denied_file, body));
    CHECK(write_file(fallback_file, body));
    CHECK(write_file(importer_file,
                     "syntax = \"proto2\";\n"
                     "import \"shared.proto\";\n"
                     "message Importer {}\n"));

    if (!deny_read_access(denied_file)) {
        SKIP("deny ACE unsupported on this filesystem");
    }

    // Precondition of every assertion below: the denial must be real, so
    // the negative results cannot be about something else (a missing file,
    // a sharing conflict, or a typo in the fixture).
    std::string contents;
    std::string read_error;
    CHECK(!easypb_file::read_disk_file(denied_file, contents, read_error));
    CHECK(read_error.find("read access is denied") != std::string::npos);

    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(denied_root);
    options.proto_paths.push_back(healthy_root);

    // 1. An input resolved through the first root fails there instead of
    //    silently loading the second root's file with the same name.
    std::vector<std::string> roots(1, "shared.proto");
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    CHECK(!easypb_proto::load_source_files(options, roots, files, error));
    CHECK(error.message.find("read access is denied") != std::string::npos);
    CHECK(files.files().empty());
    CHECK(files.targets().empty());

    // 2. The same rule for an import, with the importing file named in
    //    the message.
    std::vector<std::string> import_roots(1, "importer.proto");
    easypb_schema::SchemaSet import_files;
    easypb_schema::Diagnostic import_error;
    CHECK(!easypb_proto::load_source_files(options, import_roots,
                                          import_files, import_error));
    CHECK(import_error.message.find("importer.proto") != std::string::npos);
    CHECK(import_error.message.find("read access is denied") !=
          std::string::npos);
    CHECK(import_files.files().empty());
    CHECK(import_files.targets().empty());

    // Cleanup: restore access first, because the deny-only DACL also
    // denies deletion. The directory name carries the process id, so a
    // run that dies early cannot collide with the next one.
    CHECK(allow_full_access(denied_file));
    remove_if_present(denied_file);
    remove_if_present(importer_file);
    remove_if_present(fallback_file);
    remove_dir_if_present(denied_root);
    remove_dir_if_present(healthy_root);
}
#endif

void test_root_deduplication_and_repeated_input()
{
    // A repeated search root and a repeated input are both accepted
    // silently: the logical-name key loads each file once, like the
    // descriptor pool of the reference compiler.
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(join_arg(fixture_dir, "diamond"));
    options.proto_paths.push_back(join_arg(fixture_dir, "diamond"));
    std::vector<std::string> roots;
    roots.push_back("root.proto");
    roots.push_back("root.proto");
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    CHECK(easypb_proto::load_source_files(options, roots, files, error));
    CHECK(files.files().size() == 4);
    CHECK(files.targets().size() == 1);
}

void test_missing_unused_import_fails()
{
    require_work_root();
    const std::string dir = work_path("missing-unused");
    // The absent file must really be absent: a stale copy from an
    // interrupted run would turn the expected failure into a success.
    remove_if_present(join_arg(dir, "absent.proto"));
    CHECK(write_file(join_arg(dir, "root.proto"),
                     "syntax = \"proto2\";\n"
                     "import \"absent.proto\";\n"
                     "message Root { optional int32 x = 1; }\n"));
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(dir);
    std::vector<std::string> roots(1, "root.proto");
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    CHECK(!easypb_proto::load_source_files(options, roots, files, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_MISSING_IMPORT);
    CHECK(error.message.find("absent.proto") != std::string::npos);
    CHECK(error.message.find("root.proto") != std::string::npos);
    CHECK(error.message.find(dir) != std::string::npos ||
          error.message.find("-I") != std::string::npos);
    CHECK(files.files().empty());
}

void test_missing_weak_import_fails()
{
    // The source frontend enforces weak dependencies, exactly like the
    // reference compiler's Importer does: a weak import is an error, not
    // an optional edge.
    require_work_root();
    const std::string dir = work_path("missing-weak");
    remove_if_present(join_arg(dir, "absent-weak.proto"));
    CHECK(write_file(join_arg(dir, "root.proto"),
                     "syntax = \"proto2\";\n"
                     "import weak \"absent-weak.proto\";\n"
                     "message Root { optional int32 x = 1; }\n"));
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(dir);
    std::vector<std::string> roots(1, "root.proto");
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    CHECK(!easypb_proto::load_source_files(options, roots, files, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_MISSING_IMPORT);
    CHECK(error.message.find("absent-weak.proto") != std::string::npos);
    CHECK(error.message.find("weak") != std::string::npos);
    CHECK(files.files().empty());
}

void test_missing_public_import_fails()
{
    require_work_root();
    const std::string dir = work_path("missing-public");
    remove_if_present(join_arg(dir, "absent-public.proto"));
    CHECK(write_file(join_arg(dir, "root.proto"),
                     "syntax = \"proto2\";\n"
                     "import public \"absent-public.proto\";\n"
                     "message Root { optional int32 x = 1; }\n"));
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(dir);
    std::vector<std::string> roots(1, "root.proto");
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    CHECK(!easypb_proto::load_source_files(options, roots, files, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_MISSING_IMPORT);
    CHECK(error.message.find("absent-public.proto") != std::string::npos);
    CHECK(error.message.find("public") != std::string::npos);
    CHECK(files.files().empty());
}

void test_invalid_syntax_preserves_chain()
{
    require_work_root();
    const std::string dir = work_path("bad-syntax");
    CHECK(write_file(join_arg(dir, "broken.proto"),
                     "syntax = \"proto2\";\nmessage Broken { optional int32 x = ; }\n"));
    CHECK(write_file(join_arg(dir, "root.proto"),
                     "syntax = \"proto2\";\n"
                     "import \"broken.proto\";\n"
                     "message Root { optional int32 x = 1; }\n"));
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(dir);
    std::vector<std::string> roots(1, "root.proto");
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    CHECK(!easypb_proto::load_source_files(options, roots, files, error));
    CHECK(error.message.find("broken.proto") != std::string::npos);
    CHECK(error.message.find("root.proto") != std::string::npos);
    // W-61: the original file/line/column survive in the outer Diagnostic,
    // and the message names the logical file only once. The renderer supplies
    // the `file:line:column:` prefix; the wrapper must not embed it again.
    CHECK(error.location.line == 2);
    CHECK(error.location.column != 0);
    CHECK(error.file == "broken.proto");
    {
        std::ostringstream duplicated;
        duplicated << "broken.proto:" << error.location.line << ":"
                   << error.location.column;
        CHECK(error.message.find(duplicated.str()) == std::string::npos);
    }
    CHECK(files.files().empty());
}

void test_self_cycle_via_files()
{
    require_work_root();
    const std::string dir = work_path("cycle-self");
    CHECK(write_file(join_arg(dir, "a.proto"),
                     "syntax = \"proto2\";\n"
                     "import \"a.proto\";\n"
                     "message A { optional int32 x = 1; }\n"));
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(dir);
    std::vector<std::string> roots(1, "a.proto");
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    CHECK(!easypb_proto::load_source_files(options, roots, files, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_IMPORT_CYCLE);
    std::size_t occurrences = 0;
    std::size_t position = 0;
    while ((position = error.message.find("a.proto", position)) !=
           std::string::npos) {
        ++occurrences;
        position += 7;
    }
    CHECK(occurrences >= 2);
    CHECK(files.files().empty());
}

void test_two_node_cycle_via_files()
{
    require_work_root();
    const std::string dir = work_path("cycle-two");
    CHECK(write_file(join_arg(dir, "a.proto"),
                     "syntax = \"proto2\";\n"
                     "import \"b.proto\";\n"
                     "message A { optional int32 x = 1; }\n"));
    CHECK(write_file(join_arg(dir, "b.proto"),
                     "syntax = \"proto2\";\n"
                     "import \"a.proto\";\n"
                     "message B { optional int32 x = 1; }\n"));
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(dir);
    std::vector<std::string> roots(1, "a.proto");
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    CHECK(!easypb_proto::load_source_files(options, roots, files, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_IMPORT_CYCLE);
    CHECK(error.message.find("a.proto") != std::string::npos);
    CHECK(error.message.find("b.proto") != std::string::npos);
    CHECK(files.files().empty());
}

void test_nested_cycle_reports_root_chain()
{
    require_work_root();
    const std::string dir = work_path("nested-cycle");
    CHECK(write_file(join_arg(dir, "root.proto"),
                     "syntax = \"proto2\";\n"
                     "import \"middle.proto\";\n"
                     "message Root {}\n"));
    CHECK(write_file(join_arg(dir, "middle.proto"),
                     "syntax = \"proto2\";\n"
                     "import \"a.proto\";\n"
                     "message Middle {}\n"));
    CHECK(write_file(join_arg(dir, "a.proto"),
                     "syntax = \"proto2\";\n"
                     "import \"b.proto\";\n"
                     "message A {}\n"));
    CHECK(write_file(join_arg(dir, "b.proto"),
                     "syntax = \"proto2\";\n"
                     "import \"a.proto\";\n"
                     "message B {}\n"));
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(dir);
    std::vector<std::string> roots(1, "root.proto");
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    CHECK(!easypb_proto::load_source_files(options, roots, files, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_IMPORT_CYCLE);
    CHECK(error.message.find(
        "root.proto -> middle.proto -> a.proto -> b.proto -> a.proto") !=
        std::string::npos);
    CHECK(files.files().empty());
}

void test_invalid_import_spelling_is_rejected()
{
    // A ".." component is invalid in an import on every platform, like
    // the virtual-path rule of the reference compiler.
    require_work_root();
    const std::string dir = work_path("invalid-spelling");
    CHECK(write_file(join_arg(dir, "root.proto"),
                     "syntax = \"proto2\";\n"
                     "import \"../outside.proto\";\n"
                     "message Root { optional int32 x = 1; }\n"));
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(dir);
    std::vector<std::string> roots(1, "root.proto");
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    CHECK(!easypb_proto::load_source_files(options, roots, files, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_IMPORT);
    CHECK(error.message.find("outside.proto") != std::string::npos);
    CHECK(files.files().empty());
}

void test_distinct_files_load_together()
{
    // Distinct physical files under distinct logical names load together
    // in requested target order.
    require_work_root();
    const std::string dir = work_path("distinct-files");
    const std::string sub_a = join_arg(dir, "root-a");
    const std::string sub_b = join_arg(dir, "root-b");
    CHECK(write_file(join_arg(sub_a, "shared.proto"),
                     "syntax = \"proto2\";\nmessage Shared {}\n"));
    CHECK(write_file(join_arg(sub_b, "other.proto"),
                     "syntax = \"proto2\";\nmessage Shared {}\n"));
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(sub_a);
    options.proto_paths.push_back(sub_b);
    std::vector<std::string> roots;
    roots.push_back("shared.proto");
    roots.push_back("other.proto");
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    CHECK(easypb_proto::load_source_files(options, roots, files, error));
    CHECK(files.files().size() == 2);
    CHECK(files.targets().size() == 2);
    // Targets keep the first-requested CLI order.
    if (files.targets().size() == 2) {
        CHECK(text(files.targets()[0]->file.name) == "shared.proto");
        CHECK(text(files.targets()[1]->file.name) == "other.proto");
    }
}

// Creates 'link' as an alias of existing file 'target' (hardlink on
// Windows where symlinks often need privilege, symlink elsewhere).
// Returns false when the platform refuses, in which case the caller
// SKIPs instead of failing. Removes any stale link first so repeated
// runs never depend on leftover state. The symlink target is absolute:
// a relative target would resolve from the link's directory and leave
// a dangling link whose missing-file error would mask the scenario.
bool make_link(const std::string& target, const std::string& link)
{
#ifdef _WIN32
    std::vector<wchar_t> wide_target, wide_link;
    if (!wide_from_utf8(target, wide_target) ||
        !wide_from_utf8(link, wide_link)) {
        return false;
    }
    DeleteFileW(&wide_link[0]);
    if (CreateHardLinkW(&wide_link[0], &wide_target[0], 0) == 0) {
        return false;
    }
    return path_is_file(link);
#else
    ::unlink(link.c_str());
    const std::string absolute_target =
        easypb_file::lexical_absolute(target);
    if (::symlink(absolute_target.c_str(), link.c_str()) != 0) return false;
    return path_is_file(link);
#endif
}

void test_two_names_of_one_file_load_twice()
{
    // The loader keys files by logical name only and never compares file
    // identity, exactly like the descriptor pool of the reference
    // compiler: one physical file reached under two names (hardlink,
    // symlink, or any other alias) loads twice. The duplicate symbols
    // that follow from that are reported by the linking step, not here.
    require_work_root();
    const std::string dir = work_path("alias-pair");
    const std::string target = join_arg(dir, "real.proto");
    const std::string link = join_arg(dir, "alias.proto");
    CHECK(write_file(target,
                     "syntax = \"proto2\";\nmessage Real {}\n"));
    if (!make_link(target, link)) {
        SKIP("alias pair: cannot create a link");
    }
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(dir);
    std::vector<std::string> roots;
    roots.push_back("real.proto");
    roots.push_back("alias.proto");
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    CHECK(easypb_proto::load_source_files(options, roots, files, error));
    CHECK(files.files().size() == 2);
    CHECK(files.targets().size() == 2);
    CHECK(files.find_file("real.proto") != 0);
    CHECK(files.find_file("alias.proto") != 0);
}

void test_spaces_in_paths()
{
    require_work_root();
    const std::string dir = work_path("space dir/inner");
    CHECK(write_file(join_arg(dir, "shared.proto"),
                     "syntax = \"proto2\";\nmessage Shared {}\n"));
    CHECK(write_file(join_arg(dir, "root.proto"),
                     "syntax = \"proto2\";\n"
                     "import \"shared.proto\";\n"
                     "message Root { optional int32 x = 1; }\n"));
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(dir);
    std::vector<std::string> roots(1, "root.proto");
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    CHECK(easypb_proto::load_source_files(options, roots, files, error));
    CHECK(files.files().size() == 2);
}

void test_empty_search_roots_mean_working_directory()
{
    // An empty proto_paths list must behave exactly like an explicit
    // ["."]: the logical root below resolves against the process
    // working directory in both configurations.
    if (work_root.empty() || work_root[0] != '.') {
        SKIP("cwd default: custom work directory");
    }
    require_work_root();
    std::string base = work_root;
    if (base.compare(0, 2, "./") == 0 || base.compare(0, 2, ".\\") == 0) {
        base.erase(0, 2);
    }
    const std::string dir = work_path("cwd-case");
    CHECK(write_file(join_arg(dir, "leaf.proto"),
                     "syntax = \"proto2\";\nmessage Leaf {}\n"));
    const std::string logical = base + "/cwd-case/root.proto";
    CHECK(write_file(join_arg(dir, "root.proto"),
                     "syntax = \"proto2\";\n"
                     "import \"" + base + "/cwd-case/leaf.proto\";\n"
                     "message Root {}\n"));
    std::vector<std::string> roots(1, logical);

    easypb_proto::SourceLoaderOptions implicit;
    easypb_schema::SchemaSet implicit_files;
    easypb_schema::Diagnostic implicit_error;
    CHECK(easypb_proto::load_source_files(implicit, roots, implicit_files,
                                          implicit_error));
    CHECK(implicit_files.files().size() == 2);
    CHECK(implicit_files.targets().size() == 1);

    easypb_proto::SourceLoaderOptions explicit_dot;
    explicit_dot.proto_paths.push_back(".");
    easypb_schema::SchemaSet explicit_files;
    easypb_schema::Diagnostic explicit_error;
    CHECK(easypb_proto::load_source_files(explicit_dot, roots, explicit_files,
                                          explicit_error));
    CHECK(explicit_files.files().size() ==
          implicit_files.files().size());
    CHECK(explicit_files.targets().size() ==
          implicit_files.targets().size());

    // An empty entry behaves exactly like ".", not like the filesystem
    // root: both spell the working directory.
    easypb_proto::SourceLoaderOptions empty_entry;
    empty_entry.proto_paths.push_back("");
    easypb_schema::SchemaSet empty_files;
    easypb_schema::Diagnostic empty_error;
    CHECK(easypb_proto::load_source_files(empty_entry, roots, empty_files,
                                          empty_error));
    CHECK(empty_files.files().size() ==
          implicit_files.files().size());
    CHECK(empty_files.targets().size() ==
          implicit_files.targets().size());
}

#ifdef _WIN32
void test_windows_separators_in_disk_roots()
{
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(join_arg(fixture_dir, "diamond"));
    // CLI disk paths may use backslashes; the loader must accept them.
    std::string disk_root = join_arg(join_arg(fixture_dir, "diamond"),
                                     "root.proto");
    for (std::size_t i = 0; i < disk_root.size(); ++i) {
        if (disk_root[i] == '/') disk_root[i] = '\\';
    }
    std::vector<std::string> roots(1, disk_root);
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    CHECK(easypb_proto::load_source_files(options, roots, files, error));
    CHECK(files.files().size() == 4);
}

void test_case_variant_root_spelling_is_outside_roots()
{
    // The prefix mapping is byte-wise, exactly like the reference
    // compiler's: a search root spelled with a different case does not
    // contain the file, even where the filesystem itself would open it,
    // and naming that file as an argument fails instead of being
    // silently renamed.
    require_work_root();
    const std::string dir = work_path("case-root/CaseRoot");
    CHECK(write_file(join_arg(dir, "root.proto"),
                     "syntax = \"proto2\";\nmessage Root {}\n"));
    std::string variant = dir;
    const std::string needle = "CaseRoot";
    const std::size_t pos = variant.rfind(needle);
    CHECK(pos != std::string::npos);
    if (pos == std::string::npos) return;
    variant.replace(pos, needle.size(), "caseroot");

    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(variant);
    std::vector<std::string> roots(
        1, easypb_file::lexical_absolute(join_arg(dir, "root.proto")));
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    CHECK(!easypb_proto::load_source_files(options, roots, files, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_IMPORT);
    CHECK(error.message.find("outside all search roots") !=
          std::string::npos);
    CHECK(files.files().empty());
    CHECK(files.targets().empty());

    // The same file under the correctly spelled root resolves, so the
    // failure above is the spelling, not a broken fixture.
    easypb_proto::SourceLoaderOptions correct;
    correct.proto_paths.push_back(dir);
    std::vector<std::string> logical_roots(1, "root.proto");
    easypb_schema::SchemaSet resolved;
    CHECK(easypb_proto::load_source_files(correct, logical_roots, resolved,
                                          error));
    CHECK(resolved.files().size() == 1);
}
#endif

void test_failed_load_leaves_empty_result()
{
    require_work_root();
    const std::string dir = work_path("empty-on-failure");
    remove_if_present(join_arg(dir, "missing.proto"));
    CHECK(write_file(join_arg(dir, "root.proto"),
                     "syntax = \"proto2\";\n"
                     "import \"missing.proto\";\n"
                     "message Root {}\n"));
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(dir);
    std::vector<std::string> roots(1, "root.proto");
    easypb_schema::SchemaSet files;
    // Pre-fill with stale content to prove failure clears it.
    try {
        files.add_file("stale.proto");
    } catch (...) {
    }
    easypb_schema::Diagnostic error;
    CHECK(!easypb_proto::load_source_files(options, roots, files, error));
    CHECK(files.files().empty());
    CHECK(files.targets().empty());
}

void test_join_preserves_root_separator()
{
    // A search root that is itself a filesystem root must not gain a
    // second separator: "//dir/file.proto" is a UNC path on Windows.
    CHECK(easypb_file::join_disk_path("/", "dir/file.proto") ==
          "/dir/file.proto");
    CHECK(easypb_file::join_disk_path("C:/", "dir/file.proto") ==
          "C:/dir/file.proto");
    CHECK(easypb_file::join_disk_path("root-a/", "shared.proto") ==
          "root-a/shared.proto");
    CHECK(easypb_file::join_disk_path("root-a", "shared.proto") ==
          "root-a/shared.proto");
    // A root ending in a multibyte character valid in the active page
    // still gains its separator: the trailing bytes are data, never a
    // separator check shortcut.
    const std::string multibyte_root = std::string("root-\xC3\xA9");
    CHECK(easypb_file::join_disk_path(multibyte_root, "shared.proto") ==
          multibyte_root + "/shared.proto");
}

#ifdef _WIN32
void test_join_preserves_bare_drive_semantics()
{
    // A bare drive ("C:") denotes the drive's current directory, not the
    // drive root: joining must agree with lexical_absolute("C:"), the
    // same directory mapping uses.
    CHECK(easypb_file::join_disk_path("C:", "root.proto") ==
          easypb_file::lexical_absolute("C:") + "/root.proto");
}
#endif

void test_strip_search_root_prefix()
{
    // Pure string-level mapping used for disk-path roots: a root that
    // already ends in '/' (filesystem roots, UNC shares) matches
    // without a doubled separator.
    std::string relative;
    CHECK(easypb_file::strip_search_root_prefix("/a.proto", "/", relative));
    CHECK(relative == "a.proto");
    CHECK(easypb_file::strip_search_root_prefix("C:/d/f.proto", "C:/",
                                                relative));
    CHECK(relative == "d/f.proto");
    CHECK(easypb_file::strip_search_root_prefix("//host/share/d/f.proto",
                                                "//host/share/", relative));
    CHECK(relative == "d/f.proto");
    CHECK(easypb_file::strip_search_root_prefix("/r/a.proto", "/r",
                                                relative));
    CHECK(relative == "a.proto");
    // Outside the root, or the root itself, never maps.
    CHECK(!easypb_file::strip_search_root_prefix("/other.proto", "/r",
                                                 relative));
    CHECK(!easypb_file::strip_search_root_prefix("/r", "/r", relative));
    CHECK(!easypb_file::strip_search_root_prefix("/rooted.proto", "/other",
                                                 relative));
    // U+00AD SOFT HYPHEN is significant in file names: "a<AD>b" and
    // "<AD>ab" are different directories on every platform, so neither
    // may map under the other. These spellings stay byte-level on
    // purpose: they exercise the comparison itself.
    CHECK(!easypb_file::strip_search_root_prefix("/r/a\xAD" "b/f.proto",
                                                 "/r/\xAD" "ab", relative));
    CHECK(!easypb_file::strip_search_root_prefix("/r/\xAD" "ab/f.proto",
                                                 "/r/a\xAD" "b", relative));
#ifdef _WIN32
    // Case variants of a root spelling do not map, exactly like the
    // byte-wise ApplyMapping of the reference compiler: the filesystem
    // may well open the same file, but the mapping is textual.
    CHECK(!easypb_file::strip_search_root_prefix("C:/Dir/File.proto",
                                                 "c:/dir", relative));
    CHECK(easypb_file::strip_search_root_prefix("C:/dir/File.proto",
                                                "C:/dir", relative));
    CHECK(relative == "File.proto");
#endif
}

#ifdef _WIN32
void test_absolute_forms_resolve_lexically()
{
    // Drive-relative ("C:x") and rooted ("\x") spellings must resolve to
    // fully qualified paths so equivalent spellings compare equal during
    // logical mapping instead of producing a false "outside" answer.
    const std::string drive = fixture_dir.substr(0, 2);
    const std::string drive_relative = drive + "win-drive-relative-probe.proto";
    const std::string drive_full =
        easypb_file::lexical_absolute(drive_relative);
    CHECK(drive_full != drive_relative);
    CHECK(drive_full.size() >= 3);
    CHECK(drive_full[1] == ':');
    CHECK(drive_full[2] == '/' || drive_full[2] == '\\');
    const std::string rooted = "\\win-rooted-probe.proto";
    const std::string rooted_full = easypb_file::lexical_absolute(rooted);
    CHECK(rooted_full != rooted);
    CHECK(rooted_full.size() >= 3);
    CHECK(rooted_full[1] == ':');
    // Normalization is stable and shared by equivalent spellings.
    CHECK(easypb_file::lexical_absolute(drive_relative) ==
          easypb_file::lexical_absolute(drive_full));
    CHECK(easypb_file::lexical_absolute(rooted) ==
          easypb_file::lexical_absolute(rooted_full));
    // A bare drive root keeps its slash instead of degrading to the
    // drive-relative spelling, so later joins cannot wander into the
    // per-drive current directory.
    CHECK(easypb_file::lexical_absolute("C:/") == "C:/");
    CHECK(easypb_file::lexical_absolute("C:/dir/..") == "C:/");
    CHECK(easypb_file::join_disk_path(
        easypb_file::lexical_absolute("C:/"), "leaf.proto") ==
        "C:/leaf.proto");
}

void test_separator_normalization_preserves_multibyte()
{
    // UTF-8 continuation bytes can never equal ASCII backslash, so
    // normalization may change only actual separator bytes and must leave
    // the multibyte filename component byte-identical.
    const std::string leaf = std::string("na\xC3\xAFve");
    CHECK(easypb_file::lexical_absolute(
        std::string("C:\\dir\\") + leaf + "\\f.proto") ==
        std::string("C:/dir/") + leaf + "/f.proto");
    CHECK(easypb_file::lexical_absolute(
        std::string("C:\\dir\\") + leaf + "\\") ==
        std::string("C:/dir/") + leaf);
}

void test_wide_cli_conversion_is_unambiguous()
{
    // Starting from wchar_t argv makes the mapping deterministic: two
    // distinct Unicode strings must stay two distinct UTF-8 spellings
    // before any loader/path code sees them.
    const wchar_t e_acute[] = { static_cast<wchar_t>(0x00E9u), L'\0' };
    const wchar_t mojibake[] = { static_cast<wchar_t>(0x00C3u),
                                 static_cast<wchar_t>(0x00A9u), L'\0' };
    std::string e_utf8;
    std::string mojibake_utf8;
    CHECK(easypb_file::utf8_from_wide(e_acute, e_utf8));
    CHECK(easypb_file::utf8_from_wide(mojibake, mojibake_utf8));
    CHECK(e_utf8 == utf8_from_code_point(0x00E9u));
    CHECK(mojibake_utf8 == utf8_from_code_point(0x00C3u) +
                           utf8_from_code_point(0x00A9u));
    CHECK(e_utf8 != mojibake_utf8);

    // The internal representation supports valid Unicode paths only.
    // A lone UTF-16 surrogate must fail instead of being replaced by U+FFFD.
    const wchar_t invalid_high[] = {
        static_cast<wchar_t>(0xD800u), L'\0'
    };
    const wchar_t invalid_low[] = {
        static_cast<wchar_t>(0xDC00u), L'\0'
    };
    const wchar_t broken_pair[] = {
        static_cast<wchar_t>(0xD800u), L'A', L'\0'
    };
    std::string invalid_utf8;
    CHECK(!easypb_file::utf8_from_wide(invalid_high, invalid_utf8));
    CHECK(!easypb_file::utf8_from_wide(invalid_low, invalid_utf8));
    CHECK(!easypb_file::utf8_from_wide(broken_pair, invalid_utf8));

    const wchar_t supplementary[] = {
        static_cast<wchar_t>(0xD83Du), static_cast<wchar_t>(0xDE00u), L'\0'
    };
    std::string supplementary_utf8;
    CHECK(easypb_file::utf8_from_wide(supplementary, supplementary_utf8));
    CHECK(supplementary_utf8 == "\xF0\x9F\x98\x80");
}

// True when the process ANSI code page cannot represent this UTF-8 spelling,
// so a narrow (A-API) call would name a different entry. The conversion uses
// WC_NO_BEST_FIT_CHARS, which substitutes '?' for every character the code
// page lacks instead of guessing a lookalike, and the bytes are converted
// back to compare them with the original text.
bool ansi_spelling_loses_information(const std::string& utf8_text)
{
    std::vector<wchar_t> wide;
    if (!wide_from_utf8(utf8_text, wide)) return true;
    const int length = static_cast<int>(wide.size()) - 1;
    if (length <= 0) return false;
    const int needed =
        WideCharToMultiByte(CP_ACP, 0, &wide[0], length, 0, 0, 0, 0);
    if (needed <= 0) return true;
    std::string narrow(static_cast<std::size_t>(needed), '\0');
    BOOL used_default = FALSE;
    const int written =
        WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, &wide[0], length,
                            &narrow[0], needed, 0, &used_default);
    if (used_default != FALSE) return true;
    if (written <= 0 || written > needed) return true;
    narrow.resize(static_cast<std::size_t>(written));
    // Round trip through the code page: bytes that come back as the same
    // Unicode text mean the page can express the name after all.
    const int back =
        MultiByteToWideChar(CP_ACP, 0, narrow.c_str(), written, 0, 0);
    if (back != length) return true;
    std::vector<wchar_t> restored(static_cast<std::size_t>(back));
    if (MultiByteToWideChar(CP_ACP, 0, narrow.c_str(), written, &restored[0],
                            back) != back) {
        return true;
    }
    for (int i = 0; i < back; ++i) {
        if (restored[static_cast<std::size_t>(i)] !=
            wide[static_cast<std::size_t>(i)]) {
            return true;
        }
    }
    return false;
}

// True when the ANSI (narrow) spelling of this UTF-8 path reaches an entry.
// Used only as a control for the test below: where the code page cannot
// express the spelling, this must answer false while the loader, which
// converts to UTF-16 at the W-API boundary, still opens the real file.
bool ansi_spelling_resolves(const std::string& utf8_path)
{
    std::vector<wchar_t> wide;
    if (!wide_from_utf8(utf8_path, wide)) return false;
    const int length = static_cast<int>(wide.size()) - 1;
    if (length <= 0) return false;
    const int needed =
        WideCharToMultiByte(CP_ACP, 0, &wide[0], length, 0, 0, 0, 0);
    if (needed <= 0) return false;
    std::string narrow(static_cast<std::size_t>(needed), '\0');
    BOOL used_default = FALSE;
    const int written =
        WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, &wide[0], length,
                            &narrow[0], needed, 0, &used_default);
    if (used_default != FALSE || written <= 0) return false;
    narrow.resize(static_cast<std::size_t>(written));
    return GetFileAttributesA(narrow.c_str()) != INVALID_FILE_ATTRIBUTES;
}

void test_non_ascii_paths_do_not_depend_on_the_process_code_page()
{
    // The Windows guarantee of this loader is that a UTF-8 spelling becomes
    // UTF-16 exactly at the W-API calls (the io_win32 layer of the reference
    // compiler), so a non-ASCII path opens under any process code page. The
    // suite exercises non-ASCII names elsewhere; what this test adds is the
    // premise. It asks the system which ANSI code page is in effect and, when
    // that page cannot even spell the fixture, uses the narrow spelling as a
    // control that must stay unreachable: where the file is visible to the
    // wide API and invisible to the narrow one, a successful load cannot have
    // gone through the A-API. No manifest is involved, so the binary runs
    // under the same code page a user's codegen.exe does.
    require_work_root();
    const UINT code_page = GetACP();
    const std::string greek = utf8_from_code_point(0x03C0u);    // pi
    const std::string cyrillic = utf8_from_code_point(0x0416u); // Zhe
    const std::string dir =
        work_path(std::string("codepage-") + greek + cyrillic);
    const std::string leaf_name =
        std::string("caf") + utf8_from_code_point(0x00E9u) + ".proto";
    const std::string root_name = "root.proto";
    const std::string leaf_disk = join_arg(dir, leaf_name);
    CHECK(write_file(leaf_disk,
                     "syntax = \"proto2\";\nmessage Leaf {}\n"));
    CHECK(write_file(join_arg(dir, root_name),
                     std::string("syntax = \"proto2\";\nimport \"") +
                         leaf_name + "\";\nmessage Root {}\n"));
    std::cout << "NOTE active ANSI code page " << code_page
              << " for the non-ASCII loader test\n";

    // The predicates have to distinguish a representable spelling from an
    // unrepresentable one, otherwise the checks below are a tautology. A pure
    // ASCII name always survives the code page, whatever page is in effect,
    // and for a path the page can express the narrow probe must agree with the
    // filesystem; the condition keeps that check honest where even the scratch
    // directory lies outside the code page.
    CHECK(!ansi_spelling_loses_information("root.proto"));
    const std::string ascii_disk = work_path("codepage-ascii.proto");
    CHECK(write_file(ascii_disk, "syntax = \"proto2\";\nmessage Probe {}\n"));
    if (!ansi_spelling_loses_information(ascii_disk)) {
        CHECK(ansi_spelling_resolves(ascii_disk));
    }
    const bool outside_code_page =
        ansi_spelling_loses_information(dir) ||
        ansi_spelling_loses_information(leaf_disk);
    if (outside_code_page) {
        // The file is there for the wide API and out of reach for the narrow
        // one. Flip either line and the load below proves nothing.
        CHECK(path_is_file(leaf_disk));
        CHECK(!ansi_spelling_resolves(leaf_disk));
        CHECK(!ansi_spelling_resolves(dir));
    } else {
        std::cout << "NOTE code page " << code_page
                  << " can spell the fixture, so the narrow control does not "
                     "apply and this run is weaker evidence\n";
    }

    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(dir);
    std::vector<std::string> roots(1, root_name);
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    CHECK(easypb_proto::load_source_files(options, roots, files, error));
    CHECK(files.files().size() == 2);
    CHECK(files.targets().size() == 1);
    CHECK(files.find_file(leaf_name) != 0);
    if (files.find_file(leaf_name) != 0) {
        CHECK(files.find_file(leaf_name)->physical_name == leaf_disk);
    }

    // The same tree reached through a disk-path argument, which is the shape a
    // CLI passes: it must map to the logical name under the non-ASCII root.
    easypb_schema::SchemaSet direct;
    easypb_schema::Diagnostic direct_error;
    std::vector<std::string> direct_roots(1, join_arg(dir, root_name));
    CHECK(easypb_proto::load_source_files(options, direct_roots, direct,
                                          direct_error));
    CHECK(direct.files().size() == 2);
    CHECK(direct.find_file(leaf_name) != 0);
}

// Long-path fixture helpers. Every other fixture in this file is written
// through the plain wide spelling, which Win32 caps at MAX_PATH, and these
// calls are the exception because the tree they address is longer than that
// cap by construction. They use the loader's own conversion
// (easypb_file::wide_win32_path) rather than the plain one, so the file
// write and every query below are proof that the prefixed spelling works,
// and no other helper in this file has to know about it. Directory creation
// is the one call that would tolerate the long path anyway, which is why
// nothing here treats a created directory as evidence on its own.
bool long_path_mkdir(const std::string& disk_path)
{
    std::vector<wchar_t> wide;
    if (!easypb_file::wide_win32_path(disk_path, wide) || wide.empty()) {
        return false;
    }
    wide.push_back(L'\0');
    if (CreateDirectoryW(&wide[0], 0)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

bool long_path_write(const std::string& disk_path,
                     const std::string& contents)
{
    std::vector<wchar_t> wide;
    if (!easypb_file::wide_win32_path(disk_path, wide) || wide.empty()) {
        return false;
    }
    wide.push_back(L'\0');
    HANDLE handle = CreateFileW(&wide[0], GENERIC_WRITE, 0, 0, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, 0);
    if (handle == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok =
        WriteFile(handle, contents.data(),
                  static_cast<DWORD>(contents.size()), &written, 0) &&
        written == static_cast<DWORD>(contents.size());
    CloseHandle(handle);
    return ok != FALSE;
}

bool long_path_exists(const std::string& disk_path)
{
    std::vector<wchar_t> wide;
    if (!easypb_file::wide_win32_path(disk_path, wide) || wide.empty()) {
        return false;
    }
    wide.push_back(L'\0');
    return GetFileAttributesW(&wide[0]) != INVALID_FILE_ATTRIBUTES;
}

// The control: the same spelling with no prefix anywhere, which is what a
// filesystem call saw before long-path support. Past MAX_PATH it must fail
// to reach the entry; where it does not, the load below is weaker evidence
// and says so instead of passing silently.
bool long_path_exists_without_prefix(const std::string& disk_path)
{
    std::vector<wchar_t> wide;
    if (!wide_from_utf8(disk_path, wide)) return false;
    return GetFileAttributesW(&wide[0]) != INVALID_FILE_ATTRIBUTES;
}

// Code units the plain (unprefixed) wide spelling occupies without its
// terminator. MAX_PATH is exactly the cap that spelling runs into.
std::size_t plain_wide_length(const std::string& disk_path)
{
    std::vector<wchar_t> wide;
    if (!wide_from_utf8(disk_path, wide) || wide.empty()) return 0;
    return wide.size() - 1;
}

void long_path_remove_file(const std::string& disk_path)
{
    std::vector<wchar_t> wide;
    if (!easypb_file::wide_win32_path(disk_path, wide) || wide.empty()) {
        return;
    }
    wide.push_back(L'\0');
    DeleteFileW(&wide[0]);
}

void long_path_remove_dir(const std::string& disk_path)
{
    std::vector<wchar_t> wide;
    if (!easypb_file::wide_win32_path(disk_path, wide) || wide.empty()) {
        return;
    }
    wide.push_back(L'\0');
    RemoveDirectoryW(&wide[0]);
}

// Working-directory helpers for the test below. Setting it moves the whole
// process, and every other test resolves its fixtures relative to the CTest
// working directory, so a caller must put it back before returning.
bool set_working_directory(const std::string& disk_path)
{
    std::vector<wchar_t> wide;
    if (!wide_from_utf8(disk_path, wide)) return false;
    return SetCurrentDirectoryW(&wide[0]) != FALSE;
}

// The working directory as the platform reports it, UTF-8, without a trailing
// separator. Empty means the query could not answer.
std::string current_working_directory_text()
{
    const DWORD needed = GetCurrentDirectoryW(0, 0);
    if (needed == 0) return std::string();
    std::vector<wchar_t> buffer(needed);
    const DWORD length = GetCurrentDirectoryW(needed, &buffer[0]);
    if (length == 0 || length >= needed) return std::string();
    std::string text;
    if (!easypb_file::utf8_from_wide(&buffer[0], text)) return std::string();
    return text;
}

// Disk spelling of the first `count` components of a fixture chain below the
// work directory.
std::string chain_path(const std::vector<std::string>& parts, std::size_t count)
{
    std::string result = work_root;
    for (std::size_t i = 0; i < count; ++i) {
        result = join_arg(result, parts[i]);
    }
    return result;
}

// Logical spelling of the same chain, the way an import below the work
// directory names it.
std::string chain_logical(const std::vector<std::string>& parts)
{
    std::string result;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) result += "/";
        result += parts[i];
    }
    return result;
}

void test_wide_conversion_applies_the_long_path_prefix()
{
    // The conversion contract of codegen/file_paths.hpp, checked without
    // touching the filesystem: a drive spelling gets the prefix, a network
    // spelling gets the UNC variant (the form the reference layer loses
    // while normalizing), and a spelling that already carries the prefix is
    // translated to the plain form and re-prefixed canonically instead of
    // keeping its mixed separators. No loader run can cover the network form,
    // because a share is not available to tests.
    std::vector<wchar_t> wide;
    CHECK(easypb_file::wide_win32_path("C:/deep/dir/leaf.proto", wide));
    CHECK(std::wstring(&wide[0], wide.size()) ==
          L"\\\\?\\C:\\deep\\dir\\leaf.proto");
    CHECK(easypb_file::wide_win32_path("//host/share/leaf.proto", wide));
    CHECK(std::wstring(&wide[0], wide.size()) ==
          L"\\\\?\\UNC\\host\\share\\leaf.proto");
    CHECK(easypb_file::wide_win32_path("\\\\?\\C:/mixed\\path.proto", wide));
    CHECK(std::wstring(&wide[0], wide.size()) ==
          L"\\\\?\\C:\\mixed\\path.proto");
    CHECK(easypb_file::wide_win32_path("//?/C:/mixed/path.proto", wide));
    CHECK(std::wstring(&wide[0], wide.size()) ==
          L"\\\\?\\C:\\mixed\\path.proto");
    CHECK(easypb_file::wide_win32_path("\\\\?\\UNC\\host\\share\\x", wide));
    CHECK(std::wstring(&wide[0], wide.size()) ==
          L"\\\\?\\UNC\\host\\share\\x");
    // Backslashes come in and come out normalized.
    CHECK(easypb_file::wide_win32_path("C:\\deep\\leaf.proto", wide));
    CHECK(std::wstring(&wide[0], wide.size()) ==
          L"\\\\?\\C:\\deep\\leaf.proto");
    // An empty spelling stays empty, and invalid input is refused rather
    // than converted to something that names a different entry.
    CHECK(easypb_file::wide_win32_path("", wide));
    CHECK(wide.empty());
    CHECK(!easypb_file::wide_win32_path(std::string("bad-") + char(0xFF) +
                                           ".proto",
                                       wide));
    CHECK(!easypb_file::wide_win32_path(
        std::string("nul") + char(0) + "x.proto", wide));
}

void test_non_ascii_paths_beyond_max_path()
{
    // Win32 only hands a path to the object manager when it carries the
    // long-path prefix documented in codegen/file_paths.hpp; without it
    // every filesystem call is capped at MAX_PATH. This test loads a tree
    // whose leaf lies past that cap and whose spelling is non-ASCII in
    // every component the loader has to touch, first through a search root
    // by logical name and then by disk-path argument. Both runs go through
    // the same conversion, so a single dropped prefix fails them.
    require_work_root();
    const std::string greek = utf8_from_code_point(0x03C0u);    // pi
    const std::string cyrillic = utf8_from_code_point(0x0416u); // Zhe
    const std::string leaf_name = "na" + utf8_from_code_point(0x00EFu) +
                                  "ve-caf" +
                                  utf8_from_code_point(0x00E9u) + ".proto";

    // Grow the chain until the leaf's plain spelling passes the cap. The
    // loop rather than a fixed depth keeps that premise true whatever the
    // checkout and the build directory are called; a handful of parts is
    // enough from a path like this one.
    std::vector<std::string> parts(1, "longpath-" + greek + cyrillic);
    for (int index = 1; index <= 40; ++index) {
        const std::string probe =
            join_arg(chain_path(parts, parts.size()), leaf_name);
        if (plain_wide_length(easypb_file::lexical_absolute(probe)) >=
            MAX_PATH) {
            break;
        }
        char number[32];
#if defined(_MSC_VER)
        _snprintf(number, sizeof(number), "%d", index);
#else
        std::snprintf(number, sizeof(number), "%d", index);
#endif
        parts.push_back(std::string("deep-") + number + "-" +
                        std::string(30, 'x'));
    }

    const std::string deepest = chain_path(parts, parts.size());
    const std::string leaf_disk = join_arg(deepest, leaf_name);
    const std::string leaf_absolute = easypb_file::lexical_absolute(leaf_disk);
    const std::size_t leaf_length = plain_wide_length(leaf_absolute);
    std::cout << "NOTE deep fixture: leaf is " << leaf_length
              << " code units below " << parts.size()
              << " directories, MAX_PATH is " << MAX_PATH << '\n';
    CHECK(leaf_length >= MAX_PATH);

    for (std::size_t i = 1; i <= parts.size(); ++i) {
        CHECK(long_path_mkdir(chain_path(parts, i)));
    }
    CHECK(long_path_write(leaf_disk,
                          "syntax = \"proto2\";\nmessage Deep {}\n"));
    CHECK(long_path_exists(leaf_disk));

    // The control. Where the unprefixed spelling still reaches the leaf, the
    // machine accepts long paths regardless of the prefix and this run only
    // shows the tree is loadable.
    const bool plain_reaches = long_path_exists_without_prefix(leaf_absolute);
    if (plain_reaches) {
        std::cout << "NOTE the unprefixed spelling is reachable here, so this "
                     "run is weaker evidence\n";
    } else {
        CHECK(!plain_reaches);
    }
    std::string status_error;
    CHECK(easypb_file::disk_file_status(leaf_disk, status_error) ==
          easypb_file::FILE_STATUS_FILE);

    // A short root importing the deep file by its long logical name: the
    // deep spelling is composed by the loader, not handed to it.
    const std::string logical_import =
        chain_logical(parts) + "/" + leaf_name;
    const std::string root_disk = work_path("longpath-root.proto");
    CHECK(write_file(root_disk, std::string("syntax = \"proto2\";\nimport \"") +
                                   logical_import + "\";\nmessage Root {}\n"));
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(work_root);
    std::vector<std::string> roots(1, "longpath-root.proto");
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    CHECK(easypb_proto::load_source_files(options, roots, files, error));
    CHECK(files.files().size() == 2);
    CHECK(files.targets().size() == 1);
    CHECK(files.find_file(logical_import) != 0);
    if (files.find_file(logical_import) != 0) {
        // physical_name keeps the UTF-8 spelling of the deep disk path.
        CHECK(files.find_file(logical_import)->physical_name == leaf_disk);
    }

    // The same leaf as a disk-path argument under the deepest directory as
    // its root, which is the shape a CLI passes for a single file.
    easypb_proto::SourceLoaderOptions deep_options;
    deep_options.proto_paths.push_back(deepest);
    std::vector<std::string> deep_roots(1, leaf_disk);
    easypb_schema::SchemaSet deep_files;
    easypb_schema::Diagnostic deep_error;
    CHECK(easypb_proto::load_source_files(deep_options, deep_roots, deep_files,
                                          deep_error));
    CHECK(deep_files.files().size() == 1);
    CHECK(deep_files.targets().size() == 1);
    CHECK(deep_files.find_file(leaf_name) != 0);

    // Cleanup runs deepest first: a directory cannot go while it still
    // holds the entry below it, and the deep parts only answer to the
    // prefixed spelling either.
    long_path_remove_file(leaf_disk);
    CHECK(!long_path_exists(leaf_disk));
    for (std::size_t i = parts.size(); i > 0; --i) {
        long_path_remove_dir(chain_path(parts, i));
    }
    CHECK(!long_path_exists(deepest));
    remove_if_present(root_disk);
}

void test_trailing_dot_and_space_names_stay_literal()
{
    // Win32 trims trailing dots and spaces before it looks a name up, so a
    // spelling such as "dot.proto." used to resolve to the differently named
    // "dot.proto", or to nothing at all. The long-path prefix switches that
    // trimming off, which is why the reference compiler applies the prefix to
    // every path and not only to long ones (bazelbuild/bazel#2935). The rule
    // this test pins is the one the prefix leaves behind: a trailing dot or
    // space is part of the name, so the literal spelling is a schema file and
    // the trimmed spelling is simply absent. protoc 30.1 answers the same way
    // on the same fixture: the literal spellings compile, the trimmed ones
    // fail with "No such file or directory".
    require_work_root();
    const std::string dir = work_path("trailing-names");
    CHECK(long_path_mkdir(dir));
    const std::string dot_literal = join_arg(dir, "dot.proto.");
    const std::string dot_trimmed = join_arg(dir, "dot.proto");
    const std::string space_literal = join_arg(dir, "space.proto ");
    const std::string space_trimmed = join_arg(dir, "space.proto");
    // Only the prefixed spelling can create these: the plain call trims the
    // name on the way to the filesystem and would create a different entry.
    CHECK(long_path_write(dot_literal,
                          "syntax = \"proto2\";\nmessage Dot {}\n"));
    CHECK(long_path_write(space_literal,
                          "syntax = \"proto2\";\nmessage Space {}\n"));
    CHECK(long_path_exists(dot_literal));
    CHECK(long_path_exists(space_literal));

    // The control, and the premise of the checks below: the unprefixed
    // spelling cannot reach either literal entry. Where it can, the machine
    // does not trim at all and this run is weaker evidence rather than a
    // silent pass.
    const std::string dot_absolute = easypb_file::lexical_absolute(dot_literal);
    const std::string space_absolute =
        easypb_file::lexical_absolute(space_literal);
    const bool plain_reaches =
        long_path_exists_without_prefix(dot_absolute) ||
        long_path_exists_without_prefix(space_absolute);
    if (plain_reaches) {
        std::cout << "NOTE the unprefixed spelling reaches the literal names "
                     "here, so this run is weaker evidence\n";
    } else {
        CHECK(!long_path_exists_without_prefix(dot_absolute));
        CHECK(!long_path_exists_without_prefix(space_absolute));
    }

    // The status layer answers for the literal names and never invents the
    // trimmed ones, which is where the wrong-file risk used to sit.
    std::string status_error;
    CHECK(easypb_file::disk_file_status(dot_literal, status_error) ==
          easypb_file::FILE_STATUS_FILE);
    CHECK(easypb_file::disk_file_status(space_literal, status_error) ==
          easypb_file::FILE_STATUS_FILE);
    CHECK(easypb_file::disk_file_status(dot_trimmed, status_error) ==
          easypb_file::FILE_STATUS_ABSENT);
    CHECK(easypb_file::disk_file_status(space_trimmed, status_error) ==
          easypb_file::FILE_STATUS_ABSENT);

    // A short root reaches both literal names, by logical name and by
    // disk-path argument, exactly as it reaches an ordinary file.
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(work_root);
    const std::string dot_logical = std::string("trailing-names/") +
                                    "dot.proto.";
    std::vector<std::string> dot_roots(1, dot_logical);
    easypb_schema::SchemaSet dot_files;
    easypb_schema::Diagnostic dot_error;
    CHECK(easypb_proto::load_source_files(options, dot_roots, dot_files,
                                          dot_error));
    CHECK(dot_files.files().size() == 1);
    CHECK(dot_files.targets().size() == 1);
    if (!dot_files.targets().empty()) {
        CHECK(text(dot_files.targets()[0]->file.name) == dot_logical);
    }

    const std::string space_logical = std::string("trailing-names/") +
                                      "space.proto ";
    easypb_proto::SourceLoaderOptions space_options;
    space_options.proto_paths.push_back(dir);
    std::vector<std::string> space_roots(1, space_literal);
    easypb_schema::SchemaSet space_files;
    easypb_schema::Diagnostic space_error;
    CHECK(easypb_proto::load_source_files(space_options, space_roots,
                                          space_files, space_error));
    CHECK(space_files.files().size() == 1);
    CHECK(space_files.find_file("space.proto ") != 0);

    // An import spelled with the literal name reaches it through the parser,
    // and the trimmed spelling fails instead of resolving to another file.
    // Imports are resolved against the search roots rather than against the
    // importing file's directory, so the import names the full logical path.
    const std::string ok_root = join_arg(dir, "literal-root.proto");
    CHECK(write_file(ok_root,
                     "syntax = \"proto2\";\nimport "
                     "\"trailing-names/dot.proto.\";\n"
                     "message LiteralRoot {}\n"));
    std::vector<std::string> ok_inputs(1, "trailing-names/literal-root.proto");
    easypb_schema::SchemaSet chained;
    easypb_schema::Diagnostic chained_error;
    const bool chained_ok =
        easypb_proto::load_source_files(options, ok_inputs, chained,
                                        chained_error);
    if (!chained_ok) {
        // The literal import is the point of this test, so a failure must
        // say what the loader objected to instead of only that it failed.
        std::cerr << "literal-name import failed: " << chained_error.file
                  << ':' << chained_error.location.line << ": "
                  << chained_error.message << '\n';
    }
    CHECK(chained_ok);
    CHECK(chained.files().size() == 2);
    CHECK(chained.targets().size() == 1);
    CHECK(chained.find_file(dot_logical) != 0);

    const std::string trimmed_root = join_arg(dir, "trimmed-root.proto");
    CHECK(write_file(trimmed_root,
                     "syntax = \"proto2\";\nimport "
                     "\"trailing-names/dot.proto\";\n"
                     "message TrimmedRoot {}\n"));
    std::vector<std::string> trimmed_inputs(1,
                                            "trailing-names/trimmed-root.proto");
    easypb_schema::SchemaSet trimmed_files;
    easypb_schema::Diagnostic trimmed_error;
    CHECK(!easypb_proto::load_source_files(options, trimmed_inputs,
                                           trimmed_files, trimmed_error));
    CHECK(trimmed_files.files().empty());

    // Cleanup, and with it the proof that the prefix also deletes what it
    // created: the plain call would leave both entries behind.
    long_path_remove_file(dot_literal);
    long_path_remove_file(space_literal);
    remove_if_present(ok_root);
    remove_if_present(trimmed_root);
    long_path_remove_dir(dir);
    CHECK(!long_path_exists(dot_literal));
    CHECK(!long_path_exists(space_literal));
}

void test_relative_spelling_follows_the_working_directory()
{
    // A relative spelling is made absolute by joining the working directory, so
    // that join has to survive a directory name the ANSI code page cannot spell
    // and it has to stop where the platform stops. Win32 refuses to make the
    // working directory longer than MAX_PATH at all - measured below rather
    // than assumed - which is why the long-directory variant of this branch is
    // exercised through a long *relative* spelling in the deep-path test
    // instead. Everything here that moves the directory moves it back before
    // the next check, because the rest of the suite resolves its fixtures
    // relative to the CTest working directory.
    require_work_root();
    const std::string saved = current_working_directory_text();
    if (saved.empty()) {
        std::cout << "NOTE the working directory could not be read, so the "
                     "relative-resolution checks are skipped\n";
        return;
    }
    const std::string greek = utf8_from_code_point(0x03C0u);    // pi
    const std::string cyrillic = utf8_from_code_point(0x0416u); // Zhe
    const std::string dir = work_path(std::string("cwd-") + greek + cyrillic);
    const std::string leaf_name = "na" + utf8_from_code_point(0x00EFu) +
                                  "ve.proto";
    const std::string leaf_disk = join_arg(dir, leaf_name);
    const std::string root_disk = join_arg(dir, "root.proto");
    CHECK(write_file(leaf_disk,
                     "syntax = \"proto2\";\nmessage Leaf {}\n"));
    CHECK(write_file(root_disk, std::string("syntax = \"proto2\";\nimport \"") +
                                   leaf_name + "\";\nmessage Root {}\n"));

    // Expectations captured before the move: both names resolve against the
    // working directory that is about to change.
    const std::string dir_absolute = easypb_file::lexical_absolute(dir);
    const std::string leaf_absolute = easypb_file::lexical_absolute(leaf_disk);

    CHECK(set_working_directory(dir));
    // The wide query has to report the non-ASCII directory, converted to UTF-8.
    const std::string moved_to = current_working_directory_text();
    CHECK(!moved_to.empty());
    CHECK(easypb_file::lexical_absolute(moved_to) == dir_absolute);
    // The join itself, twice: the current directory and a bare relative name
    // both have to come out as the absolute spellings captured above. Decoding
    // the working directory through the code page instead of UTF-8 fails both,
    // which is what makes the non-ASCII fixture directory worth having.
    CHECK(easypb_file::lexical_absolute(".") == dir_absolute);
    CHECK(easypb_file::lexical_absolute(leaf_name) == leaf_absolute);
    std::string entry_error;
    CHECK(easypb_file::disk_file_status(leaf_name, entry_error) ==
          easypb_file::FILE_STATUS_FILE);

    // The loader, driven the two ways a relative spelling reaches it: an empty
    // search root, which means the working directory, and a relative disk-path
    // argument.
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back("");
    std::vector<std::string> chain_roots(1, "root.proto");
    easypb_schema::SchemaSet chain;
    easypb_schema::Diagnostic chain_error;
    CHECK(easypb_proto::load_source_files(options, chain_roots, chain,
                                          chain_error));
    CHECK(chain.files().size() == 2);
    CHECK(chain.targets().size() == 1);
    CHECK(chain.find_file(leaf_name) != 0);

    std::vector<std::string> leaf_roots(1, leaf_name);
    easypb_schema::SchemaSet only;
    easypb_schema::Diagnostic only_error;
    CHECK(easypb_proto::load_source_files(options, leaf_roots, only,
                                          only_error));
    CHECK(only.files().size() == 1);
    CHECK(only.targets().size() == 1);
    if (!only.targets().empty()) {
        CHECK(text(only.targets()[0]->file.name) == leaf_name);
    }

    // Back before anything else, so that a failure above cannot leave the rest
    // of the suite resolving its fixtures somewhere else.
    CHECK(set_working_directory(saved));
    CHECK(current_working_directory_text() == saved);

    // The platform limit, measured here so the suite does not assume it. One
    // directory with a very long name is enough to cross it, and the prefixed
    // spelling is not accepted for a working directory at all, which is why
    // this uses SetCurrentDirectoryW directly.
    const std::string deep = work_path(std::string(240, 'd'));
    CHECK(long_path_mkdir(deep));
    const std::size_t deep_length =
        plain_wide_length(easypb_file::lexical_absolute(deep));
    const bool deep_accepted = set_working_directory(deep);
    std::cout << "NOTE a working directory of " << deep_length
              << " code units is " << (deep_accepted ? "accepted" : "refused")
              << " (MAX_PATH is " << MAX_PATH << ")\n";
    if (deep_accepted) {
        // A platform that allows this makes the branch testable, so take it.
        CHECK(easypb_file::lexical_absolute("leaf.proto") ==
              join_arg(easypb_file::lexical_absolute(deep), "leaf.proto"));
        CHECK(set_working_directory(saved));
    } else {
        // A refused move must leave the process where it was, or every later
        // relative fixture path in this suite would resolve differently.
        CHECK(current_working_directory_text() == saved);
    }

    long_path_remove_dir(deep);
    remove_if_present(root_disk);
    remove_if_present(leaf_disk);
    remove_dir_if_present(dir);
}
#endif

void test_utf8_logical_spelling_rules()
{
    // Logical names are strict UTF-8. Structural checks stay byte-exact
    // because UTF-8 continuation bytes never collide with ASCII separators.
    // This is the deliberate strictness beyond the reference compiler,
    // which applies its virtual-path rules only when opening a file.
    std::string reason;
    CHECK(easypb_schema::validate_logical_path("a/b.proto", reason));
    CHECK(!easypb_schema::validate_logical_path("a\\b.proto", reason));
    CHECK(!easypb_schema::validate_logical_path("C:/b.proto", reason));
    CHECK(!easypb_schema::validate_logical_path("bad'name.proto", reason));
    CHECK(!easypb_schema::validate_logical_path("", reason));
    const std::string leaf = std::string("na\xC3\xAFve.proto");
    CHECK(easypb_schema::validate_logical_path(leaf, reason));
    CHECK(!easypb_schema::validate_logical_path(
        std::string("bad-") + char(0xFF) + ".proto", reason));
    CHECK(reason.find("UTF-8") != std::string::npos);
    CHECK(!easypb_schema::validate_logical_path(
        std::string("bad-") + char(0xC0) + char(0xAF) + ".proto", reason));
    CHECK(!easypb_schema::validate_logical_path(
        std::string("bad-") + char(0xED) + char(0xA0) + char(0x80) +
            ".proto",
        reason));
}

void test_non_ascii_filenames_round_trip()
{
    // Test spellings are UTF-8 on every platform and files are created
    // through the same spelling the loader resolves, so a write/read
    // round trip with identical bytes must work everywhere.
    // "na\xc3\xafve.proto" is naive-with-diaeresis.proto in UTF-8.
    require_work_root();
    const std::string dir = work_path("encoding");
    const std::string leaf_name = std::string("na\xC3\xAFve.proto");
    const std::string root_name = std::string("caf\xC3\xA9.proto");
    CHECK(write_file(join_arg(dir, leaf_name),
                     "syntax = \"proto2\";\nmessage Leaf {}\n"));
    CHECK(write_file(join_arg(dir, root_name),
                     std::string("syntax = \"proto2\";\nimport \"") +
                     leaf_name + "\";\nmessage Root {}\n"));
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(dir);
    std::vector<std::string> roots(1, root_name);
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    CHECK(easypb_proto::load_source_files(options, roots, files, error));
    CHECK(files.files().size() == 2);
    CHECK(files.targets().size() == 1);
    if (!files.targets().empty()) {
        CHECK(text(files.targets()[0]->file.name) == root_name);
    }
    CHECK(files.find_file(leaf_name) != 0);
}

void test_utf8_import_finds_unicode_file()
{
    // One logical namespace: a file created for its Unicode name must
    // resolve through a UTF-8 import spelling on every process code
    // page, and the SchemaSet key must be the UTF-8 spelling. (On
    // Windows the fixture goes through the W API and is read back
    // through it; POSIX is UTF-8 natively.)
    require_work_root();
    const std::string dir = work_path("unicode-import");
    const std::string leaf_name =
        std::string("caf") + utf8_from_code_point(0x00E9u) + ".proto";
    const std::string root_name = "root.proto";
    CHECK(write_file(join_arg(dir, leaf_name),
                     "syntax = \"proto2\";\nmessage Caf {}\n"));
    CHECK(write_file(join_arg(dir, root_name),
                     std::string("syntax = \"proto2\";\nimport \"") +
                     leaf_name + "\";\nmessage Root {}\n"));
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(dir);
    std::vector<std::string> roots(1, root_name);
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    CHECK(easypb_proto::load_source_files(options, roots, files, error));
    CHECK(files.files().size() == 2);
    CHECK(files.targets().size() == 1);
    CHECK(files.find_file(leaf_name) != 0);
    CHECK(files.find_file(root_name) != 0);
}

void test_unicode_search_root_uses_wide_filesystem_boundary()
{
    require_work_root();
    const std::string mark = utf8_from_code_point(0x00E9u);
    const std::string dir = work_path(std::string("root-caf") + mark);
    CHECK(write_file(join_arg(dir, "root.proto"),
                     "syntax = \"proto2\";\nmessage Root {}\n"));
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(dir);
    std::vector<std::string> roots(1, "root.proto");
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    CHECK(easypb_proto::load_source_files(options, roots, files, error));
    CHECK(files.find_file("root.proto") != 0);
}

void test_non_ascii_trail_character_path()
{
    // U+2015 historically encodes as CP932 81 5C, but no CP932 bytes ever
    // enter production now. The UTF-8 spelling must survive separator
    // normalization, joining and W-API file access as an ordinary
    // character.
    require_work_root();
    const std::string mark = utf8_from_code_point(0x2015u);
    const std::string dir = work_path(std::string("trail-") + mark);
    const std::string file = join_arg(dir, "root.proto");
    CHECK(write_file(file, "syntax = \"proto2\";\nmessage Root {}\n"));
    const std::string absolute = easypb_file::lexical_absolute(dir + "/./");
    CHECK(path_is_file(easypb_file::join_disk_path(absolute, "root.proto")));
}

void test_nested_duplicate_is_rejected()
{
    // A repeated name in one dependency list is rejected by the shared
    // graph binder. The loader no longer decorates binder errors with the
    // discovery chain (that decoration was part of the removed
    // identity-era diagnostics); loader-level errors, such as a missing
    // import or a denied access, still carry the chain.
    require_work_root();
    const std::string dir = work_path("nested-duplicate");
    CHECK(write_file(join_arg(dir, "root.proto"),
                     "syntax = \"proto2\";\n"
                     "import \"child.proto\";\n"
                     "message Root {}\n"));
    CHECK(write_file(join_arg(dir, "child.proto"),
                     "syntax = \"proto2\";\n"
                     "import \"leaf.proto\";\n"
                     "import \"leaf.proto\";\n"
                     "message Child {}\n"));
    CHECK(write_file(join_arg(dir, "leaf.proto"),
                     "syntax = \"proto2\";\nmessage Leaf {}\n"));
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(dir);
    std::vector<std::string> roots(1, "root.proto");
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    CHECK(!easypb_proto::load_source_files(options, roots, files, error));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_IMPORT);
    CHECK(error.file == "child.proto");
    CHECK(error.message.find("duplicate import") != std::string::npos);
    CHECK(files.files().empty());
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2 || argv[1][0] == '\0') {
        std::cerr << "usage: test_loader <fixture-data-dir> [work-dir]\n"
                  << "When work-dir is omitted, ./imports-loader-work under\n"
                  << "the current working directory is used. Run via ctest\n"
                  << "so scratch stays under the build tree; for manual\n"
                  << "runs pass an explicit work-dir under build/.\n";
        return EXIT_FAILURE;
    }
    fixture_dir = argv[1];
    if (argc >= 3 && argv[2][0] != '\0') work_root = argv[2];
    if (!path_is_file(join_arg(fixture_dir, "diamond/root.proto"))) {
        std::cerr << "fixture directory lacks diamond/root.proto: "
                  << fixture_dir << '\n';
        return EXIT_FAILURE;
    }
    require_work_root();

    test_diamond_caching();
    test_search_precedence_first_wins();
#ifndef _WIN32
    test_denied_access_stops_search();
    test_nested_denied_import_preserves_context();
#endif
    test_nul_byte_in_disk_path_fails_loudly();
    test_nul_byte_in_import_fails();
    test_shadowed_root_reports_error();
    test_disk_root_with_invalid_logical_name_fails();
    test_directory_argument_is_reported_as_directory();
    test_empty_input_argument_is_rejected();
    test_root_deduplication_and_repeated_input();
#ifdef _WIN32
    // The POSIX denial test below relies on chmod, so Windows needs its
    // own ACL-based counterpart to cover the same rule.
    test_denied_access_stops_search();
#endif
    test_missing_unused_import_fails();
    test_missing_weak_import_fails();
    test_missing_public_import_fails();
    test_invalid_syntax_preserves_chain();
    test_self_cycle_via_files();
    test_two_node_cycle_via_files();
    test_nested_cycle_reports_root_chain();
    test_invalid_import_spelling_is_rejected();
    test_distinct_files_load_together();
    test_two_names_of_one_file_load_twice();
    test_spaces_in_paths();
    test_empty_search_roots_mean_working_directory();
    test_failed_load_leaves_empty_result();
    test_join_preserves_root_separator();
    test_strip_search_root_prefix();
    test_utf8_logical_spelling_rules();
    test_non_ascii_filenames_round_trip();
    test_utf8_import_finds_unicode_file();
    test_unicode_search_root_uses_wide_filesystem_boundary();
    test_non_ascii_trail_character_path();
    test_nested_duplicate_is_rejected();
#ifdef _WIN32
    test_windows_separators_in_disk_roots();
    test_case_variant_root_spelling_is_outside_roots();
    test_join_preserves_bare_drive_semantics();
    test_absolute_forms_resolve_lexically();
    test_separator_normalization_preserves_multibyte();
    test_wide_cli_conversion_is_unambiguous();
    test_non_ascii_paths_do_not_depend_on_the_process_code_page();
    test_wide_conversion_applies_the_long_path_prefix();
    test_non_ascii_paths_beyond_max_path();
    test_trailing_dot_and_space_names_stay_literal();
    test_relative_spelling_follows_the_working_directory();
#endif

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all loader tests passed";
    if (skips != 0) std::cout << " (" << skips << " skipped)";
    std::cout << "\n";
    return EXIT_SUCCESS;
}
