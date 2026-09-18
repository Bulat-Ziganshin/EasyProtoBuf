// Real filesystem discovery and caching tests for the source loader
// (codegen/proto_loader.hpp/.cpp plus file_paths). Full builds only.
// Permanent fixtures live under tests/codegen/imports/data/loader/ and
// remain unchanged; files created here go under a work directory below
// the CTest working directory pinned in tests/CMakeLists.txt (the CMake
// tests binary directory) and are rewritten on every run so repeated
// runs never depend on stale data.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef EASYPB_EXTENDED_TESTS
#include <atomic>
#include <functional>
#include <thread>
#endif

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
#include <winioctl.h>
#else
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

int failures = 0;
int skips = 0;

int status_probe_root_a_calls = 0;
int status_probe_root_b_calls = 0;
int nested_status_error_probe_calls = 0;
#ifdef _WIN32
int case_query_error_probe_calls = 0;

int forced_case_query_operational_error(const std::string& directory)
{
    (void)directory;
    ++case_query_error_probe_calls;
    return -2;
}

std::string confirm_error_marker;
int confirm_error_probe_calls = 0;

// Fails identity queries for the root-spelled containment candidate
// while succeeding (with a dummy valid identity) everywhere else, so
// the confirmation path meets "identity says error, status says file".
// The marker matches the root spelling exactly; the fold-variant
// argument spelling differs in case and passes through.
bool failing_confirm_identity_probe(const std::string& path,
                                    easypb_file::FileIdentity& identity,
                                    std::string& error)
{
    if (!confirm_error_marker.empty() &&
        path.find(confirm_error_marker) != std::string::npos) {
        ++confirm_error_probe_calls;
        error = "synthetic confirm identity failure";
        return false;
    }
    identity.valid = true;
    identity.key_a = 1;
    identity.key_b = 2;
    return true;
}
#endif

easypb_file::FileStatus first_root_error_probe(const std::string& path,
                                               std::string& error)
{
    if (path.find("probe-root-a") != std::string::npos) {
        ++status_probe_root_a_calls;
        error = "synthetic root-a metadata failure";
        return easypb_file::FILE_STATUS_ERROR;
    }
    if (path.find("probe-root-b") != std::string::npos) {
        ++status_probe_root_b_calls;
        return easypb_file::FILE_STATUS_FILE;
    }
    return easypb_file::FILE_STATUS_ABSENT;
}

easypb_file::FileStatus nested_import_error_probe(const std::string& path,
                                                  std::string& error)
{
    ++nested_status_error_probe_calls;
    // The CLI root is a logical name. Do not make the initial disk-path
    // convenience probe claim that a bare "root.proto" exists in CWD.
    if (path == "root.proto") return easypb_file::FILE_STATUS_ABSENT;
    if (path.find("leaf.proto") != std::string::npos) {
        error = "synthetic leaf metadata failure";
        return easypb_file::FILE_STATUS_ERROR;
    }
    return easypb_file::FILE_STATUS_FILE;
}
#ifdef EASYPB_EXTENDED_TESTS
std::string extended_fault_marker;
std::string extended_file_marker;
int extended_fault_probe_calls = 0;
int extended_file_probe_calls = 0;

easypb_file::FileStatus extended_search_fault_probe(const std::string& path,
                                                    std::string& error)
{
    // A bare logical root is first tested as a disk-path convenience.
    if (path == "root.proto") return easypb_file::FILE_STATUS_ABSENT;
    if (!extended_fault_marker.empty() &&
        path.find(extended_fault_marker) != std::string::npos) {
        ++extended_fault_probe_calls;
        error = "synthetic extended search-root failure";
        return easypb_file::FILE_STATUS_ERROR;
    }
    if (!extended_file_marker.empty() &&
        path.find(extended_file_marker) != std::string::npos) {
        ++extended_file_probe_calls;
        return easypb_file::FILE_STATUS_FILE;
    }
    return easypb_file::FILE_STATUS_ABSENT;
}
#endif

void check(bool condition, const char* expression, const char* file, int line)
{
    if (!condition) {
        std::cerr << file << ':' << line << ": CHECK failed: " << expression << '\n';
        ++failures;
    }
}

#define CHECK(x) check((x), #x, __FILE__, __LINE__)

#ifdef _WIN32
// Converts a UTF-8 test spelling for a Win32 W call. Windows loader paths
// use the same UTF-8 representation and never depend on the process ANSI
// code page.
bool wide_from_utf8(const std::string& text, std::vector<wchar_t>& wide);
#endif

// Creates 'link' as an alias of existing file 'target' (defined below).
bool make_link(const std::string& target, const std::string& link);

#ifdef _WIN32
// Creates 'link' as an NTFS directory junction pointing at 'target'
// (defined below). Unlike symlinks, junctions need no elevation.
bool make_junction(const std::string& target, const std::string& link);
#endif

std::string text(const str_view& value)
{
    return std::string(value.data(), value.size());
}

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

bool file_exists(const std::string& path)
{
    return easypb_file::disk_file_exists(path);
}

// Deletes a file whose absence is a test precondition. Case directories
// are reused across runs, so a stale file left by an interrupted run
// must not flip an expected failure into a success.
void remove_if_present(const std::string& path)
{
    std::remove(path.c_str());
}

// Encodes one BMP code point as UTF-8 for a Windows path spelling. The
// loader converts that spelling only at the W-API boundary.
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

void test_diamond_caching_and_counters()
{
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(join_arg(fixture_dir, "diamond"));
    std::vector<std::string> roots(1, "root.proto");
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    easypb_proto::LoadStatistics statistics;
    CHECK(easypb_proto::load_source_files(options, roots, files, error,
                                          &statistics));
    if (files.files().size() != 4) {
        CHECK(files.files().size() == 4);
        return;
    }
    CHECK(files.targets().size() == 1);
    CHECK(statistics.files_read == 4);
    CHECK(statistics.files_parsed == 4);
    // Each unique physical/logical file was read once: shared.proto is
    // reached through both left and right but parsed a single time.
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
    CHECK(easypb_proto::load_source_files(options, roots, files, error, 0));
    easypb_schema::SchemaFile* shared = files.find_file("shared.proto");
    CHECK(shared != 0);
    if (shared != 0) CHECK(text(shared->file.package) == "first");
}

void test_search_precedence_error_stops_before_later_root()
{
    status_probe_root_a_calls = 0;
    status_probe_root_b_calls = 0;
    easypb_file::set_file_status_probe(first_root_error_probe);

    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back("probe-root-a");
    options.proto_paths.push_back("probe-root-b");
    std::vector<std::string> roots(1, "shared.proto");
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    const bool ok =
        easypb_proto::load_source_files(options, roots, files, error, 0);

    easypb_file::set_file_status_probe(0);
    CHECK(!ok);
    CHECK(status_probe_root_a_calls == 1);
    CHECK(status_probe_root_b_calls == 0);
    CHECK(error.message.find("synthetic root-a metadata failure") !=
          std::string::npos);
    CHECK(files.files().empty());
    CHECK(files.targets().empty());
}

void test_nested_operational_error_preserves_import_context()
{
    require_work_root();
    const std::string dir = work_path("nested-operational-error");
    CHECK(write_file(join_arg(dir, "root.proto"),
                     "syntax = \"proto2\";\n"
                     "import \"middle.proto\";\n"
                     "message Root {}\n"));
    CHECK(write_file(join_arg(dir, "middle.proto"),
                     "syntax = \"proto2\";\n"
                     "import \"leaf.proto\";\n"
                     "message Middle {}\n"));

    nested_status_error_probe_calls = 0;
    easypb_file::set_file_status_probe(nested_import_error_probe);

    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(dir);
    std::vector<std::string> roots(1, "root.proto");
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    const bool ok =
        easypb_proto::load_source_files(options, roots, files, error, 0);

    easypb_file::set_file_status_probe(0);
    CHECK(!ok);
    CHECK(nested_status_error_probe_calls >= 3);
    CHECK(error.file == "middle.proto");
    CHECK(error.location.line == 2);
    CHECK(error.message.find("cannot resolve import \"leaf.proto\"") !=
          std::string::npos);
    CHECK(error.message.find("synthetic leaf metadata failure") !=
          std::string::npos);
    CHECK(error.message.find("searched") != std::string::npos);
    CHECK(error.message.find("\"root.proto\" -> \"middle.proto\" -> \"leaf.proto\"") !=
          std::string::npos);
    CHECK(files.files().empty());
    CHECK(files.targets().empty());
}

void test_nul_byte_in_disk_path_fails_loudly()
{
    require_work_root();
    const std::string dir = work_path("nul-path");
    CHECK(write_file(join_arg(dir, "lonely.proto"),
                     "syntax = \"proto2\";\nmessage Lonely {}\n"));

    // The file exists, but the requested spelling carries a NUL suffix
    // that native calls would truncate: the loader must fail loudly
    // instead of reporting a successful load under a confused name.
    // This is the isolated-root shape (fully qualified, import-free),
    // which previously accepted the truncated prefix as a success.
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
        easypb_proto::load_source_files(options, roots, files, error, 0);

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
        easypb_proto::load_source_files(options, roots, files, error, 0);

    CHECK(!ok);
    CHECK(error.message.find("control characters") != std::string::npos);
    CHECK(files.files().empty());
    CHECK(files.targets().empty());
}

void test_case_fold_mismatch_is_not_containment()
{
    require_work_root();
    // U+10400 Deseret capital vs U+10428 small: folded equal by the
    // locale-invariant uppercase table, different directory entries
    // for the OS. A search root through one spelling must never claim
    // the file under the other spelling as contained: the fully
    // qualified import-free root loads as isolated instead of
    // misreporting a shadow.
    const std::string small_dir =
        work_path(std::string("fold-dir-\xF0\x90\x90\xA8"));
    const std::string capital_dir =
        work_path(std::string("fold-dir-\xF0\x90\x90\x80"));
    CHECK(write_file(join_arg(small_dir, "root.proto"),
                     "syntax = \"proto2\";\nmessage Small {}\n"));
    CHECK(write_file(join_arg(capital_dir, "root.proto"),
                     "syntax = \"proto2\";\nmessage Capital {}\n"));

    const std::string argument = easypb_file::absolute_disk_path(
        join_arg(capital_dir, "root.proto"));
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(small_dir);
    std::vector<std::string> roots(1, argument);
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    const bool ok =
        easypb_proto::load_source_files(options, roots, files, error, 0);

    CHECK(ok);
    CHECK(files.files().size() == 1);
    CHECK(files.targets().size() == 1);
    CHECK(files.find_file(argument) != 0);
}

void test_fold_hardlink_alias_names_fail()
{
    // Same file under two Deseret-differing isolated names: folding
    // says equivalent, the OS holds distinct entries. Two isolated
    // roots must be a duplicate alias naming both spellings, never a
    // silent merge into one target.
    require_work_root();
    const std::string dir = work_path("fold-hardlink-alias");
    const std::string small =
        join_arg(dir, std::string("root-\xF0\x90\x90\xA8.proto"));
    const std::string capital =
        join_arg(dir, std::string("root-\xF0\x90\x90\x80.proto"));
    CHECK(write_file(small,
                     "syntax = \"proto2\";\nmessage Fold {}\n"));
    if (!make_link(small, capital)) {
        std::cout << "SKIP fold hardlink alias: cannot create link\n";
        ++skips;
        return;
    }
    const std::string absolute_small =
        easypb_file::absolute_disk_path(small);
    const std::string absolute_capital =
        easypb_file::absolute_disk_path(capital);
    // The scenario needs one physical file under two names; without a
    // real link there is nothing to alias.
    easypb_file::FileIdentity small_identity, capital_identity;
    std::string identity_error;
    if (!easypb_file::get_file_identity(absolute_small, small_identity,
                                        identity_error) ||
        !easypb_file::get_file_identity(absolute_capital,
                                        capital_identity,
                                        identity_error) ||
        !easypb_file::same_identity(small_identity, capital_identity)) {
        std::cout << "SKIP fold hardlink alias: links are distinct "
                     "files\n";
        ++skips;
        return;
    }
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(join_arg(fixture_dir, "diamond"));
    std::vector<std::string> roots;
    roots.push_back(absolute_small);
    roots.push_back(absolute_capital);
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    CHECK(!easypb_proto::load_source_files(options, roots, files, error, 0));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_IMPORT);
    CHECK(error.message.find("two logical names") != std::string::npos);
    CHECK(error.message.find(absolute_small) != std::string::npos);
    CHECK(error.message.find(absolute_capital) != std::string::npos);
    CHECK(files.files().empty());
    CHECK(files.targets().empty());
}

void test_fold_hardlink_file_stays_isolated()
{
    // Same file addressed through a fold-matching search root: entry
    // comparison rejects the containment (distinct directory entries),
    // so the fully qualified import-free root loads as isolated under
    // its original absolute name instead of "root.proto".
    require_work_root();
    const std::string small_dir =
        work_path(std::string("fold-link-small-\xF0\x90\x90\xA8"));
    const std::string capital_dir =
        work_path(std::string("fold-link-small-\xF0\x90\x90\x80"));
    CHECK(write_file(join_arg(small_dir, "root.proto"),
                     "syntax = \"proto2\";\nmessage Small {}\n"));
    // Placeholder creates the capital directory; the link replaces it.
    CHECK(write_file(join_arg(capital_dir, "root.proto"),
                     "syntax = \"proto2\";\nmessage Placeholder {}\n"));
    if (!make_link(join_arg(small_dir, "root.proto"),
                   join_arg(capital_dir, "root.proto"))) {
        std::cout << "SKIP fold hardlink isolated: cannot create link\n";
        ++skips;
        return;
    }
    const std::string argument = easypb_file::absolute_disk_path(
        join_arg(capital_dir, "root.proto"));
    easypb_file::FileIdentity argument_identity, linked_identity;
    std::string identity_error;
    if (!easypb_file::get_file_identity(
            argument, argument_identity, identity_error) ||
        !easypb_file::get_file_identity(
            easypb_file::absolute_disk_path(
                join_arg(small_dir, "root.proto")),
            linked_identity, identity_error) ||
        !easypb_file::same_identity(argument_identity, linked_identity)) {
        std::cout << "SKIP fold hardlink isolated: links are distinct "
                     "files\n";
        ++skips;
        return;
    }
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(small_dir);
    std::vector<std::string> roots(1, argument);
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    const bool ok =
        easypb_proto::load_source_files(options, roots, files, error, 0);

    CHECK(ok);
    CHECK(files.files().size() == 1);
    CHECK(files.targets().size() == 1);
    CHECK(files.find_file(argument) != 0);
}

#ifdef _WIN32
void test_junction_fold_mismatch_is_not_containment()
{
    // Two Deseret-differing junction entries to one target directory:
    // folding says equivalent, the entries are distinct. A search root
    // through one must never claim the file under the other as
    // contained: the fully qualified import-free root stays isolated
    // under its original absolute name.
    require_work_root();
    const std::string real_dir = work_path("junction-target/real");
    CHECK(write_file(join_arg(real_dir, "root.proto"),
                     "syntax = \"proto2\";\nmessage Real {}\n"));
    const std::string absolute_real =
        easypb_file::absolute_disk_path(real_dir);
    const std::string small_link = work_path(
        std::string("junction-\xF0\x90\x90\xA8"));
    const std::string capital_link = work_path(
        std::string("junction-\xF0\x90\x90\x80"));
    if (!make_junction(absolute_real, small_link) ||
        !make_junction(absolute_real, capital_link)) {
        std::cout << "SKIP junction fold containment: "
                     "cannot create links\n";
        ++skips;
        return;
    }
    CHECK(easypb_file::disk_file_exists(
        join_arg(small_link, "root.proto")));
    CHECK(easypb_file::disk_file_exists(
        join_arg(capital_link, "root.proto")));

    const std::string argument = easypb_file::absolute_disk_path(
        join_arg(capital_link, "root.proto"));
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(small_link);
    std::vector<std::string> roots(1, argument);
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    const bool ok =
        easypb_proto::load_source_files(options, roots, files, error, 0);

    CHECK(ok);
    CHECK(files.files().size() == 1);
    CHECK(files.targets().size() == 1);
    CHECK(files.find_file(argument) != 0);
}

void test_junction_fold_alias_names_fail()
{
    // Same target file under two Deseret-differing junction names:
    // distinct entries, one file. Two isolated roots must fail as a
    // duplicate alias naming both spellings, never merge silently.
    require_work_root();
    const std::string real_dir = work_path("junction-alias/real");
    CHECK(write_file(join_arg(real_dir, "root.proto"),
                     "syntax = \"proto2\";\nmessage Real {}\n"));
    const std::string absolute_real =
        easypb_file::absolute_disk_path(real_dir);
    const std::string small_link = work_path(
        std::string("junction-alias-\xF0\x90\x90\xA8"));
    const std::string capital_link = work_path(
        std::string("junction-alias-\xF0\x90\x90\x80"));
    if (!make_junction(absolute_real, small_link) ||
        !make_junction(absolute_real, capital_link)) {
        std::cout << "SKIP junction fold alias: cannot create links\n";
        ++skips;
        return;
    }
    const std::string absolute_small = easypb_file::absolute_disk_path(
        join_arg(small_link, "root.proto"));
    const std::string absolute_capital = easypb_file::absolute_disk_path(
        join_arg(capital_link, "root.proto"));
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(join_arg(fixture_dir, "diamond"));
    std::vector<std::string> roots;
    roots.push_back(absolute_small);
    roots.push_back(absolute_capital);
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    CHECK(!easypb_proto::load_source_files(options, roots, files, error, 0));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_IMPORT);
    CHECK(error.message.find("two logical names") != std::string::npos);
    CHECK(error.message.find(absolute_small) != std::string::npos);
    CHECK(error.message.find(absolute_capital) != std::string::npos);
    CHECK(files.files().empty());
    CHECK(files.targets().empty());
}

void test_junction_same_entry_case_alias()
{
    // Two ASCII case spellings of the SAME junction entry still
    // confirm as one entry: ordinary case-alias behavior through
    // junctions keeps working while distinct entries are rejected.
    require_work_root();
    const std::string real_dir = work_path("junction-case/real");
    CHECK(write_file(join_arg(real_dir, "root.proto"),
                     "syntax = \"proto2\";\nmessage Real {}\n"));
    const std::string absolute_real =
        easypb_file::absolute_disk_path(real_dir);
    const std::string link = work_path("junction-case-link");
    if (!make_junction(absolute_real, link)) {
        std::cout << "SKIP junction same entry: cannot create link\n";
        ++skips;
        return;
    }
    std::string search_root = link;
    const std::string needle = "junction-case-link";
    const std::size_t pos = search_root.rfind(needle);
    CHECK(pos != std::string::npos);
    if (pos == std::string::npos) return;
    search_root.replace(pos, needle.size(), "JUNCTION-CASE-LINK");

    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(search_root);
    std::vector<std::string> roots(1, "root.proto");
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    const bool ok =
        easypb_proto::load_source_files(options, roots, files, error, 0);

    CHECK(ok);
    CHECK(files.files().size() == 1);
    CHECK(files.targets().size() == 1);
    CHECK(files.find_file("root.proto") != 0);
}
#endif

void test_shadowed_root_reports_error()
{
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(join_arg(fixture_dir, "search/root-a"));
    options.proto_paths.push_back(join_arg(fixture_dir, "search/root-b"));
    std::vector<std::string> roots;
    roots.push_back("app.proto");
    // Explicitly naming the shadowed file must not silently rename it or
    // switch the cached import to the second package.
    roots.push_back(join_arg(join_arg(fixture_dir, "search/root-b"),
                             "shared.proto"));
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    CHECK(!easypb_proto::load_source_files(options, roots, files, error, 0));
    CHECK(error.message.find("shadow") != std::string::npos);
    CHECK(files.files().empty());
    CHECK(files.targets().empty());
}

void test_disk_root_with_invalid_logical_name_fails()
{
    // A disk-path root physically under a search root must map to a
    // valid logical name: quotes are forbidden in logical names, so the
    // remainder below is invalid and the root must fail instead of
    // falling through to the isolated absolute fallback.
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
    CHECK(!easypb_proto::load_source_files(options, roots, files, error, 0));
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
                                          error, 0));
    CHECK(ok_files.files().size() == 1);
    CHECK(ok_files.targets().size() == 1);
}

void test_root_deduplication_preserves_order()
{
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(join_arg(fixture_dir, "diamond"));
    std::vector<std::string> roots;
    roots.push_back("root.proto");
    roots.push_back("root.proto");
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    easypb_proto::LoadStatistics statistics;
    CHECK(easypb_proto::load_source_files(options, roots, files, error,
                                          &statistics));
    CHECK(files.files().size() == 4);
    CHECK(files.targets().size() == 1);
    CHECK(statistics.files_read == 4);
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
    CHECK(!easypb_proto::load_source_files(options, roots, files, error, 0));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_MISSING_IMPORT);
    CHECK(error.message.find("absent.proto") != std::string::npos);
    CHECK(error.message.find("root.proto") != std::string::npos);
    CHECK(error.message.find(dir) != std::string::npos ||
          error.message.find("-I") != std::string::npos);
    CHECK(files.files().empty());
}

void test_missing_weak_import_fails()
{
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
    CHECK(!easypb_proto::load_source_files(options, roots, files, error, 0));
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
    CHECK(!easypb_proto::load_source_files(options, roots, files, error, 0));
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
    CHECK(!easypb_proto::load_source_files(options, roots, files, error, 0));
    CHECK(error.message.find("broken.proto") != std::string::npos);
    CHECK(error.message.find("root.proto") != std::string::npos);
    // The plan requires the original file/line/column to survive: the
    // stray ";" is on line 2 of broken.proto and must appear verbatim.
    CHECK(error.location.line == 2);
    CHECK(error.location.column != 0);
    {
        std::ostringstream expected;
        expected << "broken.proto:" << error.location.line << ":"
                 << error.location.column;
        CHECK(error.message.find(expected.str()) != std::string::npos);
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
    CHECK(!easypb_proto::load_source_files(options, roots, files, error, 0));
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
    CHECK(!easypb_proto::load_source_files(options, roots, files, error, 0));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_IMPORT_CYCLE);
    CHECK(error.message.find("a.proto") != std::string::npos);
    CHECK(error.message.find("b.proto") != std::string::npos);
    CHECK(files.files().empty());
}

void test_invalid_import_spelling_is_rejected()
{
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
    CHECK(!easypb_proto::load_source_files(options, roots, files, error, 0));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_IMPORT);
    CHECK(error.message.find("outside.proto") != std::string::npos);
    CHECK(files.files().empty());
}

void test_distinct_files_load_together()
{
    // Distinct physical files under distinct logical names load together
    // in requested target order. The same-physical/two-logical alias is
    // covered by the symlink/hardlink and case-alias tests below.
    require_work_root();
    const std::string dir = work_path("duplicate-alias");
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
    CHECK(easypb_proto::load_source_files(options, roots, files, error, 0));
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
// a dangling link whose missing-file error would mask the alias check.
// A successfully created link is verified to resolve for the same reason.
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
    return easypb_file::disk_file_exists(link);
#else
    ::unlink(link.c_str());
    const std::string absolute_target =
        easypb_file::absolute_disk_path(target);
    if (::symlink(absolute_target.c_str(), link.c_str()) != 0) return false;
    return easypb_file::disk_file_exists(link);
#endif
}

#ifdef _WIN32
// Mount-point reparse buffer layout (stable ABI), defined locally:
// REPARSE_DATA_BUFFER coverage varies across lean/legacy SDK headers,
// while FSCTL codes resolve fine. Only junction creation below needs it.
struct JunctionReparseBuffer {
    DWORD tag;               // IO_REPARSE_TAG_MOUNT_POINT
    WORD data_length;        // bytes after this 8-byte header
    WORD reserved;
    WORD substitute_offset;  // bytes from path_start, no NUL counted
    WORD substitute_length;
    WORD print_offset;
    WORD print_length;
    wchar_t path_start[1];
};

bool make_junction(const std::string& target, const std::string& link)
{
    std::vector<wchar_t> wide_target, wide_link;
    if (!wide_from_utf8(target, wide_target) ||
        !wide_from_utf8(link, wide_link)) {
        return false;
    }
    // NT substitute names require the \??\ prefix and backslashes.
    // wide_from_utf8 NUL-terminates: strip the terminator first, or
    // it becomes part of the substitute name and the junction resolves
    // nowhere.
    std::wstring print(&wide_target[0],
                       &wide_target[0] + wide_target.size());
    if (!print.empty() && print[print.size() - 1] == L'\0') {
        print.erase(print.size() - 1);
    }
    for (std::size_t i = 0; i < print.size(); ++i) {
        if (print[i] == L'/') print[i] = L'\\';
    }
    const std::wstring substitute = L"\\??\\" + print;
    const DWORD sub_bytes =
        static_cast<DWORD>(substitute.size() * sizeof(wchar_t));
    const DWORD print_bytes =
        static_cast<DWORD>(print.size() * sizeof(wchar_t));
    std::vector<char> buffer(MAXIMUM_REPARSE_DATA_BUFFER_SIZE, 0);
    JunctionReparseBuffer* reparse =
        reinterpret_cast<JunctionReparseBuffer*>(&buffer[0]);
    reparse->tag = IO_REPARSE_TAG_MOUNT_POINT;
    reparse->data_length = static_cast<WORD>(
        8 + sub_bytes + sizeof(wchar_t) + print_bytes + sizeof(wchar_t));
    reparse->reserved = 0;
    reparse->substitute_offset = 0;
    reparse->substitute_length = static_cast<WORD>(sub_bytes);
    reparse->print_offset = static_cast<WORD>(sub_bytes + sizeof(wchar_t));
    reparse->print_length = static_cast<WORD>(print_bytes);
    wchar_t* path_buffer = reparse->path_start;
    std::memcpy(path_buffer, substitute.data(), sub_bytes);
    path_buffer[substitute.size()] = L'\0';
    std::memcpy(path_buffer + substitute.size() + 1, print.data(),
                print_bytes);
    path_buffer[substitute.size() + 1 + print.size()] = L'\0';
    const DWORD total = 8 + reparse->data_length;

    // Removing a junction never touches its target; a stale plain
    // directory would fail removal when non-empty and bail out below.
    RemoveDirectoryW(&wide_link[0]);
    if (!CreateDirectoryW(&wide_link[0], 0)) return false;
    HANDLE handle = CreateFileW(
        &wide_link[0], GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        0, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, 0);
    if (handle == INVALID_HANDLE_VALUE) {
        RemoveDirectoryW(&wide_link[0]);
        return false;
    }
    DWORD returned = 0;
    const BOOL set = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT,
                                     &buffer[0], total, 0, 0, &returned, 0);
    CloseHandle(handle);
    if (!set) {
        RemoveDirectoryW(&wide_link[0]);
        return false;
    }
    const DWORD attributes = GetFileAttributesW(&wide_link[0]);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}
#endif

void test_symlink_alias_detection()
{
    require_work_root();
    const std::string dir = work_path("symlink-alias");
    const std::string target = join_arg(dir, "real.proto");
    const std::string link = join_arg(dir, "alias.proto");
    CHECK(write_file(target,
                     "syntax = \"proto2\";\nmessage Real {}\n"));
    if (!make_link(target, link)) {
        std::cout << "SKIP symlink/hardlink alias: cannot create link\n";
        ++skips;
        return;
    }
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(dir);
    std::vector<std::string> roots;
    roots.push_back("real.proto");
    roots.push_back("alias.proto");
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    // One physical source under two logical names must fail and name
    // both spellings.
    CHECK(!easypb_proto::load_source_files(options, roots, files, error, 0));
    if (error.message.find("real.proto") == std::string::npos ||
        error.message.find("alias.proto") == std::string::npos) {
        CHECK(error.message.find("real.proto") != std::string::npos);
        CHECK(error.message.find("alias.proto") != std::string::npos);
    }
    CHECK(files.files().empty());
}

void test_nested_alias_reports_importer_context()
{
    // root.proto -> middle.proto, where middle.proto imports real.proto
    // and then its hardlink alias.proto on line 3: the alias conflict
    // must name the importing file, the declaration site, and the path
    // from the selected root.
    require_work_root();
    const std::string dir = work_path("nested-alias");
    CHECK(write_file(join_arg(dir, "root.proto"),
                     "syntax = \"proto2\";\n"
                     "import \"middle.proto\";\n"
                     "message Root {}\n"));
    CHECK(write_file(join_arg(dir, "middle.proto"),
                     "syntax = \"proto2\";\n"
                     "import \"real.proto\";\n"
                     "import \"alias.proto\";\n"
                     "message Middle {}\n"));
    CHECK(write_file(join_arg(dir, "real.proto"),
                     "syntax = \"proto2\";\nmessage Real {}\n"));
    if (!make_link(join_arg(dir, "real.proto"), join_arg(dir, "alias.proto"))) {
        std::cout << "SKIP nested alias: cannot create link\n";
        ++skips;
        return;
    }
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(dir);
    std::vector<std::string> roots(1, "root.proto");
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    CHECK(!easypb_proto::load_source_files(options, roots, files, error, 0));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_IMPORT);
    CHECK(error.file == "middle.proto");
    CHECK(error.location.line == 3);
    CHECK(error.message.find("real.proto") != std::string::npos);
    CHECK(error.message.find("alias.proto") != std::string::npos);
    CHECK(error.message.find(
        "\"root.proto\" -> \"middle.proto\" -> \"alias.proto\"") !=
        std::string::npos);
    CHECK(files.files().empty());
}

void test_soft_hyphen_directories_stay_distinct()
{
    // U+00AD SOFT HYPHEN is significant in file names but invisible to
    // linguistic comparison: "a<AD>b" and "<AD>ab" are different
    // directories holding different files. A search root in the first
    // must not swallow an absolute root from the second as shadowed;
    // the second file loads as an isolated root instead. The character
    // is UTF-8 encoded like every test spelling, so the test never
    // depends on the source file encoding; the check is portable
    // because POSIX stays byte-exact and Windows must not merge what
    // the filesystem keeps apart.
    require_work_root();
    const std::string soft_hyphen = utf8_from_code_point(0x00ADu);
    const std::string dir_a = work_path(std::string("a") + soft_hyphen + "b");
    const std::string dir_b = work_path(soft_hyphen + "ab");
    CHECK(write_file(join_arg(dir_a, "root.proto"),
                     "syntax = \"proto2\";\nmessage Root {}\n"));
    CHECK(write_file(join_arg(dir_b, "root.proto"),
                     "syntax = \"proto2\";\nmessage Root {}\n"));
    const std::string file_b =
        easypb_file::absolute_disk_path(join_arg(dir_b, "root.proto"));
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(dir_a);
    std::vector<std::string> roots(1, file_b);
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    CHECK(easypb_proto::load_source_files(options, roots, files, error, 0));
    CHECK(files.files().size() == 1);
    CHECK(files.targets().size() == 1);
    if (!files.targets().empty()) {
        CHECK(text(files.targets()[0]->file.name) == file_b);
    }
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
    CHECK(easypb_proto::load_source_files(options, roots, files, error, 0));
    CHECK(files.files().size() == 2);
}

void test_empty_search_roots_mean_working_directory()
{
    // An empty proto_paths list must behave exactly like an explicit
    // ["."]: the logical root below resolves against the process
    // working directory in both configurations.
    if (work_root.empty() || work_root[0] != '.') {
        std::cout << "SKIP cwd default: custom work directory\n";
        ++skips;
        return;
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
                                          implicit_error, 0));
    CHECK(implicit_files.files().size() == 2);
    CHECK(implicit_files.targets().size() == 1);

    easypb_proto::SourceLoaderOptions explicit_dot;
    explicit_dot.proto_paths.push_back(".");
    easypb_schema::SchemaSet explicit_files;
    easypb_schema::Diagnostic explicit_error;
    CHECK(easypb_proto::load_source_files(explicit_dot, roots, explicit_files,
                                          explicit_error, 0));
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
                                          empty_error, 0));
    CHECK(empty_files.files().size() ==
          implicit_files.files().size());
    CHECK(empty_files.targets().size() ==
          implicit_files.targets().size());
}

void test_windows_separators_in_disk_roots()
{
#ifdef _WIN32
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
    CHECK(easypb_proto::load_source_files(options, roots, files, error, 0));
    CHECK(files.files().size() == 4);
#else
    std::cout << "SKIP windows separators: POSIX platform\n";
    ++skips;
#endif
}

void test_case_alias_and_distinct_files()
{
    require_work_root();
    const std::string dir = work_path("case-names");
    CHECK(write_file(join_arg(dir, "shared.proto"),
                     "syntax = \"proto2\";\npackage lower;\nmessage Shared {}\n"));
    const std::string upper = join_arg(dir, "Shared.proto");
    // Try to create a distinct upper-case file. On case-insensitive
    // filesystems this overwrites the same physical file.
    const bool upper_written =
        write_file(upper, "syntax = \"proto2\";\npackage upper;\nmessage Shared {}\n");
    CHECK(upper_written);
    easypb_file::FileIdentity lower_identity;
    easypb_file::FileIdentity upper_identity;
    std::string identity_error;
    const bool lower_ok = easypb_file::get_file_identity(
        join_arg(dir, "shared.proto"), lower_identity, identity_error);
    const bool upper_ok =
        easypb_file::get_file_identity(upper, upper_identity, identity_error);
    CHECK(lower_ok && upper_ok);
    if (!lower_ok || !upper_ok) return;
    if (easypb_file::same_identity(lower_identity, upper_identity)) {
        // Case-insensitive filesystem: the two spellings alias one file
        // and must be rejected with both names explained.
        easypb_proto::SourceLoaderOptions options;
        options.proto_paths.push_back(dir);
        std::vector<std::string> roots;
        roots.push_back("shared.proto");
        roots.push_back("Shared.proto");
        easypb_schema::SchemaSet files;
        easypb_schema::Diagnostic error;
        CHECK(!easypb_proto::load_source_files(options, roots, files, error,
                                               0));
        CHECK(error.message.find("shared.proto") != std::string::npos);
        CHECK(error.message.find("Shared.proto") != std::string::npos);
    } else {
        // Case-sensitive filesystem: the two files are distinct and both
        // load together.
        easypb_proto::SourceLoaderOptions options;
        options.proto_paths.push_back(dir);
        std::vector<std::string> roots;
        roots.push_back("shared.proto");
        roots.push_back("Shared.proto");
        easypb_schema::SchemaSet files;
        easypb_schema::Diagnostic error;
        CHECK(easypb_proto::load_source_files(options, roots, files, error,
                                              0));
        CHECK(files.files().size() == 2);
    }
}

void test_isolated_absolute_root()
{
    require_work_root();
    const std::string outside = work_path("isolated-outside");
    const std::string absolute_file = join_arg(outside, "lonely.proto");
    CHECK(write_file(absolute_file,
                     "syntax = \"proto2\";\nmessage Lonely {}\n"));
    const std::string absolute =
        easypb_file::absolute_disk_path(absolute_file);
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(join_arg(fixture_dir, "diamond"));
    std::vector<std::string> roots(1, absolute);
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    CHECK(easypb_proto::load_source_files(options, roots, files, error, 0));
    CHECK(files.files().size() == 1);
    CHECK(files.targets().size() == 1);

    // The same isolated file with an import requires an explicit -I:
    // the helper lives next to the isolated file but outside the
    // configured search roots, so the diamond root alone cannot cover it.
    CHECK(write_file(join_arg(outside, "isolated-helper.proto"),
                     "syntax = \"proto2\";\nmessage Helper {}\n"));
    CHECK(write_file(absolute_file,
                     "syntax = \"proto2\";\n"
                     "import \"isolated-helper.proto\";\n"
                     "message Lonely { optional int32 x = 1; }\n"));
    easypb_schema::SchemaSet second;
    CHECK(!easypb_proto::load_source_files(options, roots, second, error, 0));
    CHECK(error.message.find("-I") != std::string::npos);
    CHECK(second.files().empty());
    // Adding the isolated directory with -I covers the helper.
    easypb_proto::SourceLoaderOptions with_helper = options;
    with_helper.proto_paths.push_back(outside);
    easypb_schema::SchemaSet third;
    CHECK(easypb_proto::load_source_files(with_helper, roots, third, error,
                                          0));
    CHECK(third.files().size() == 2);
}

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
    CHECK(!easypb_proto::load_source_files(options, roots, files, error, 0));
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
    const std::string multibyte_root =
        std::string("root-\xC3\xA9");
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
    // may map under the other even where linguistic comparison would
    // equate them. These spellings stay byte-level on purpose: they
    // exercise the comparison itself without any filesystem encoding.
    CHECK(!easypb_file::strip_search_root_prefix("/r/a\xAD" "b/f.proto",
                                                 "/r/\xAD" "ab", relative));
    CHECK(!easypb_file::strip_search_root_prefix("/r/\xAD" "ab/f.proto",
                                                 "/r/a\xAD" "b", relative));
#ifdef _WIN32
    CHECK(easypb_file::strip_search_root_prefix("C:/Dir/File.proto",
                                                "c:/dir", relative));
    CHECK(relative == "File.proto");
    // Case-equivalent spellings with different byte lengths (U+0250 is
    // two bytes in UTF-8, its uppercase U+2C6F three) must still map:
    // the prefix boundary is computed in wide space, never by cutting
    // the file at the root's byte length. The remainder keeps the
    // file's own bytes verbatim. The containing directory is real, so
    // the sensitivity probe answers instead of failing inconclusively.
    {
        const std::string lower_mark = utf8_from_code_point(0x0250u);
        const std::string upper_mark = utf8_from_code_point(0x2C6Fu);
        if (lower_mark.empty() || upper_mark.empty()) {
            std::cout << "SKIP unicode prefix: UTF-8 helper failed\n";
            ++skips;
        } else {
            require_work_root();
            const std::string parent = work_path("unicode-prefix");
            CHECK(make_dirs(parent));
            const std::string lower_root =
                join_arg(parent, std::string("dir-") + lower_mark);
            const std::string upper_root =
                join_arg(parent, std::string("dir-") + upper_mark);
            const int parent_probe =
                easypb_file::directory_case_sensitive(parent);
            const bool alias_ok =
                parent_probe != 1 && parent_probe != -2;
            const bool first_mapped =
                easypb_file::strip_search_root_prefix(
                    join_arg(upper_root, "f.proto"), lower_root, relative);
            const bool first_ok = first_mapped && relative == "f.proto";
            const bool second_mapped =
                easypb_file::strip_search_root_prefix(
                    join_arg(lower_root, "f.proto"), upper_root, relative);
            const bool second_ok = second_mapped && relative == "f.proto";
            if (alias_ok) {
                CHECK(first_ok);
                CHECK(second_ok);
            } else {
                CHECK(!first_mapped);
                CHECK(!second_mapped);
            }
        }
    }
#endif
}

#ifdef _WIN32
void test_absolute_path_normalizes_partial_forms()
{
    // Drive-relative ("C:x") and rooted ("\x") spellings must resolve to
    // fully qualified paths so equivalent roots map to one logical name
    // instead of aliasing.
    const std::string drive = fixture_dir.substr(0, 2);
    const std::string drive_relative = drive + "win-drive-relative-probe.proto";
    const std::string drive_full =
        easypb_file::absolute_disk_path(drive_relative);
    CHECK(drive_full != drive_relative);
    CHECK(drive_full.size() >= 3);
    CHECK(drive_full[1] == ':');
    CHECK(drive_full[2] == '/' || drive_full[2] == '\\');
    const std::string rooted = "\\win-rooted-probe.proto";
    const std::string rooted_full = easypb_file::absolute_disk_path(rooted);
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
    CHECK(easypb_file::is_fully_qualified_disk_path(
        easypb_file::lexical_absolute("C:/")));
    CHECK(easypb_file::join_disk_path(
        easypb_file::lexical_absolute("C:/"), "leaf.proto") ==
        "C:/leaf.proto");
}
#endif

#ifdef _WIN32
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
#endif

void test_utf8_logical_spelling_rules()
{
    // Logical names are strict UTF-8. Structural checks stay byte-exact
    // because UTF-8 continuation bytes never collide with ASCII separators.
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

#ifdef _WIN32
void test_drive_relative_root_is_not_isolated()
{
    // "C:dir\file.proto" is drive-relative (resolved against the
    // per-drive current directory), not absolute: it must not get the
    // fully qualified isolated-root convenience even when it names an
    // existing file outside all search roots.
    require_work_root();
    const std::string dir = work_path("drive-relative");
    CHECK(write_file(join_arg(dir, "lonely.proto"),
                     "syntax = \"proto2\";\nmessage Lonely {}\n"));
    const std::string absolute =
        easypb_file::lexical_absolute(join_arg(dir, "lonely.proto"));
    const std::string cwd = easypb_file::lexical_absolute(".");
    // The scenario needs the file on the process drive under the
    // working directory; otherwise no drive-relative spelling exists.
    if (absolute.size() <= cwd.size() + 1 ||
        absolute.compare(0, cwd.size(), cwd) != 0 ||
        (absolute[cwd.size()] != '/' && absolute[cwd.size()] != '\\') ||
        cwd.size() < 2 || cwd[1] != ':') {
        std::cout << "SKIP drive-relative root: file outside working "
                     "directory\n";
        ++skips;
        return;
    }
    const std::string drive_relative =
        cwd.substr(0, 2) + absolute.substr(cwd.size() + 1);
    CHECK(easypb_file::disk_file_exists(drive_relative));
    CHECK(!easypb_file::is_fully_qualified_disk_path(drive_relative));
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(join_arg(fixture_dir, "diamond"));
    std::vector<std::string> roots(1, drive_relative);
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    CHECK(!easypb_proto::load_source_files(options, roots, files, error, 0));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_IMPORT);
    CHECK(error.message.find("outside all search roots") !=
          std::string::npos);
    CHECK(files.files().empty());
    CHECK(files.targets().empty());
}
#endif

void test_isolated_root_with_resolvable_imports_fails()
{
    // Plan 3 allows the absolute-name convenience for import-free roots
    // only: an isolated root that declares imports needs an explicit -I
    // even when the imports would resolve through another search root.
    require_work_root();
    const std::string outside = work_path("isolated-resolvable");
    CHECK(write_file(join_arg(outside, "lonely.proto"),
                     "syntax = \"proto2\";\n"
                     "import \"shared.proto\";\n"
                     "message Lonely { optional int32 x = 1; }\n"));
    const std::string absolute =
        easypb_file::absolute_disk_path(join_arg(outside, "lonely.proto"));
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(join_arg(fixture_dir, "diamond"));
    std::vector<std::string> roots(1, absolute);
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    CHECK(!easypb_proto::load_source_files(options, roots, files, error, 0));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_IMPORT);
    CHECK(error.message.find("-I") != std::string::npos);
    CHECK(error.message.find("shared.proto") != std::string::npos);
    // The offending import declaration site survives: "shared.proto" is
    // imported on line 2 of the isolated root.
    CHECK(error.location.line == 2);
    CHECK(error.location.column != 0);
    CHECK(files.files().empty());
    CHECK(files.targets().empty());
}

void test_non_ascii_filenames_round_trip()
{
    // Test spellings are UTF-8 on every platform and files are created
    // through the same spelling the loader resolves, so a write/read
    // round trip with identical bytes must work everywhere.
    // "na\xc3\xafve.proto" is naïve.proto in UTF-8.
    require_work_root();
    const std::string dir = work_path("encoding");
    const std::string leaf_name = std::string("na\xc3\xafve.proto");
    const std::string root_name = std::string("caf\xc3\xa9.proto");
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
    CHECK(easypb_proto::load_source_files(options, roots, files, error, 0));
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
    CHECK(easypb_proto::load_source_files(options, roots, files, error, 0));
    CHECK(files.files().size() == 2);
    CHECK(files.targets().size() == 1);
    CHECK(files.find_file(leaf_name) != 0);
    CHECK(files.find_file(root_name) != 0);
}

void test_equivalent_isolated_roots_deduplicate()
{
    // Equivalent spellings of one isolated file ("abs/f.proto" versus
    // "abs/./f.proto") share one canonical identity; the first spelling
    // wins as the logical name instead of aliasing.
    require_work_root();
    const std::string outside = work_path("isolated-duplicate");
    CHECK(write_file(join_arg(outside, "lonely.proto"),
                     "syntax = \"proto2\";\nmessage Lonely {}\n"));
    const std::string directory =
        easypb_file::absolute_disk_path(outside);
    const std::string first = join_arg(directory, "lonely.proto");
    const std::string second = directory + "/./lonely.proto";
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(join_arg(fixture_dir, "diamond"));
    std::vector<std::string> roots;
    roots.push_back(first);
    roots.push_back(second);
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    CHECK(easypb_proto::load_source_files(options, roots, files, error, 0));
    CHECK(files.files().size() == 1);
    CHECK(files.targets().size() == 1);
    if (!files.targets().empty()) {
        CHECK(text(files.targets()[0]->file.name) == first);
    }
#ifdef _WIN32
    // The same file through "/" and "\" separators deduplicates as well.
    std::string slashed = second;
    for (std::size_t i = 0; i < slashed.size(); ++i) {
        if (slashed[i] == '/') slashed[i] = '\\';
    }
    std::vector<std::string> mixed_roots;
    mixed_roots.push_back(first);
    mixed_roots.push_back(slashed);
    easypb_schema::SchemaSet mixed;
    CHECK(easypb_proto::load_source_files(options, mixed_roots, mixed,
                                          error, 0));
    CHECK(mixed.files().size() == 1);
    CHECK(mixed.targets().size() == 1);
    // Windows disk paths are case-insensitive: a differently-cased drive
    // letter and directory name address the same physical file and must
    // deduplicate with the first spelling kept, not alias. Inside a
    // case-sensitive directory the recased spelling is a different
    // logical name of one file instead, and must alias-fail.
    std::string recased = first;
    for (std::size_t i = 0; i < recased.size(); ++i) {
        if (recased[i] >= 'a' && recased[i] <= 'z') {
            recased[i] = static_cast<char>(recased[i] - 'a' + 'A');
        } else if (recased[i] >= 'A' && recased[i] <= 'Z') {
            recased[i] = static_cast<char>(recased[i] - 'A' + 'a');
        }
    }
    if (recased != first) {
        // A positively sensitive directory (or an inconclusive query
        // on a capable system) turns the recased spelling into a
        // different logical name of one file, which must alias-fail;
        // anywhere else the spellings deduplicate to the first name.
        const int probe = easypb_file::directory_case_sensitive(directory);
        const bool alias_ok = probe != 1 && probe != -2;
        if (!alias_ok) {
            std::vector<std::string> case_roots;
            case_roots.push_back(first);
            case_roots.push_back(recased);
            easypb_schema::SchemaSet cased;
            CHECK(!easypb_proto::load_source_files(options, case_roots,
                                                   cased, error, 0));
            CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_IMPORT);
            CHECK(error.message.find("two logical names") !=
                  std::string::npos);
            CHECK(cased.files().empty());
        } else {
            std::vector<std::string> case_roots;
            case_roots.push_back(first);
            case_roots.push_back(recased);
            easypb_schema::SchemaSet cased;
            CHECK(easypb_proto::load_source_files(options, case_roots, cased,
                                                  error, 0));
            CHECK(cased.files().size() == 1);
            CHECK(cased.targets().size() == 1);
            if (!cased.targets().empty()) {
                CHECK(text(cased.targets()[0]->file.name) == first);
            }
        }
    }
#endif
}

void test_distinct_isolated_hardlink_names_fail()
{
    // Two isolated absolute roots naming one physical file through
    // different hardlink/symlink spellings are a duplicate alias, not a
    // silent deduplication: both names must be reported and nothing may
    // load. Lexically equivalent spellings (covered above) still share
    // the first name.
    require_work_root();
    const std::string dir = work_path("isolated-hardlink-alias");
    const std::string target = join_arg(dir, "real.proto");
    const std::string link = join_arg(dir, "alias.proto");
    CHECK(write_file(target,
                     "syntax = \"proto2\";\nmessage Real {}\n"));
    if (!make_link(target, link)) {
        std::cout << "SKIP isolated hardlink alias: cannot create link\n";
        ++skips;
        return;
    }
    const std::string absolute_target =
        easypb_file::absolute_disk_path(target);
    const std::string absolute_link =
        easypb_file::absolute_disk_path(link);
    // The scenario needs one physical file under two names; without a
    // real link there is nothing to alias.
    easypb_file::FileIdentity target_identity, link_identity;
    std::string identity_error;
    if (!easypb_file::get_file_identity(absolute_target, target_identity,
                                        identity_error) ||
        !easypb_file::get_file_identity(absolute_link, link_identity,
                                        identity_error) ||
        !easypb_file::same_identity(target_identity, link_identity)) {
        std::cout << "SKIP isolated hardlink alias: links are distinct "
                     "files\n";
        ++skips;
        return;
    }
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(join_arg(fixture_dir, "diamond"));
    std::vector<std::string> roots;
    roots.push_back(absolute_target);
    roots.push_back(absolute_link);
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    CHECK(!easypb_proto::load_source_files(options, roots, files, error, 0));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_IMPORT);
    CHECK(error.message.find("real.proto") != std::string::npos);
    CHECK(error.message.find("alias.proto") != std::string::npos);
    CHECK(files.files().empty());
    CHECK(files.targets().empty());
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
    CHECK(!easypb_proto::load_source_files(options, roots, files, error, 0));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_IMPORT_CYCLE);
    CHECK(error.message.find(
        "root.proto -> middle.proto -> a.proto -> b.proto -> a.proto") !=
        std::string::npos);
    CHECK(files.files().empty());
}

#ifdef _WIN32
void test_non_ascii_case_alias_matches_search_root()
{
    // Search root ".../accent-é" with an absolute root spelling
    // ".../accent-É/...": Windows opens the same physical file with the
    // same native identity, so the recased root must map instead of
    // falling out to the isolated error. Both spellings are UTF-8 like
    // every test path, so the test never depends on the source file
    // encoding.
    require_work_root();
    const std::string lower_mark = utf8_from_code_point(0x00E9u);
    const std::string upper_mark = utf8_from_code_point(0x00C9u);
    if (lower_mark.empty() || upper_mark.empty()) {
        std::cout << "SKIP non-ASCII case alias: UTF-8 helper failed\n";
        ++skips;
        return;
    }
    const std::string lower_dir =
        work_path(std::string("accent-") + lower_mark);
    CHECK(write_file(join_arg(lower_dir, "leaf.proto"),
                     "syntax = \"proto2\";\nmessage Leaf {}\n"));
    CHECK(write_file(join_arg(lower_dir, "root.proto"),
                     "syntax = \"proto2\";\n"
                     "import \"leaf.proto\";\n"
                     "message Root {}\n"));
    const std::string lower_root =
        easypb_file::absolute_disk_path(lower_dir);
    // Recase only the trailing directory component: the encoded marks
    // must not collide with parent-directory bytes.
    std::string upper_root = lower_root;
    const std::size_t last_separator =
        upper_root.find_last_of("/\\");
    const std::size_t component_start =
        last_separator == std::string::npos ? 0 : last_separator + 1;
    const std::size_t mark_at =
        upper_root.find(lower_mark, component_start);
    if (mark_at == std::string::npos) {
        std::cout << "SKIP non-ASCII case alias: unexpected root spelling\n";
        ++skips;
        return;
    }
    upper_root.replace(mark_at, lower_mark.size(), upper_mark);
    const std::string upper_file = join_arg(upper_root, "root.proto");
    // The recased spelling must address the same physical file for this
    // scenario to apply; otherwise there is nothing to deduplicate.
    easypb_file::FileIdentity lower_identity, upper_identity;
    std::string identity_error;
    if (!easypb_file::get_file_identity(join_arg(lower_root, "root.proto"),
                                        lower_identity, identity_error) ||
        !easypb_file::get_file_identity(upper_file, upper_identity,
                                        identity_error) ||
        !easypb_file::same_identity(lower_identity, upper_identity)) {
        std::cout << "SKIP non-ASCII case alias: case-sensitive setup\n";
        ++skips;
        return;
    }
    // Same file, so the search-root prefix comparison must fold the case
    // variants instead of falling out to the isolated error.
    std::string probed;
    CHECK(easypb_file::strip_search_root_prefix(upper_file, lower_root,
                                                probed));
    CHECK(probed == "root.proto");
    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(lower_dir);
    std::vector<std::string> roots(1, upper_file);
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    CHECK(easypb_proto::load_source_files(options, roots, files, error, 0));
    CHECK(files.files().size() == 2);
    CHECK(files.targets().size() == 1);
    if (!files.targets().empty()) {
        CHECK(text(files.targets()[0]->file.name) == "root.proto");
    }
}
#endif

#ifdef _WIN32
void test_case_sensitive_directory_semantics()
{
    // Case-only differences are equivalent spellings inside ordinary
    // directories but different names inside a case-sensitive one.
    // The branch taken follows the live probe, so the test holds on
    // both kinds of volumes; hosts without sensitive directories
    // exercise the insensitive branch (sensitivity cannot be enabled
    // without elevation).
    require_work_root();
    const std::string dir = work_path("case-sensitivity");
    CHECK(write_file(join_arg(join_arg(dir, "inc"), "f.proto"),
                     "syntax = \"proto2\";\nmessage F {}\n"));
    CHECK(easypb_file::directory_case_sensitive(
        join_arg(dir, "definitely-missing-dir")) != 1);
    // The "inc" versus "INC" child-name comparison is governed by the
    // parent directory; the file-name comparison below by "inc" itself.
    // A failed query on a capable system (-2) is handled conservatively
    // like a sensitive answer; only -1 (no query API) keeps history.
    const int parent_probe = easypb_file::directory_case_sensitive(dir);
    CHECK(parent_probe >= -2 && parent_probe <= 1);
    const int child_probe = easypb_file::directory_case_sensitive(
        join_arg(dir, "inc"));
    CHECK(child_probe >= -2 && child_probe <= 1);
    const std::string child = join_arg(dir, "inc");
    const std::string root = easypb_file::absolute_disk_path(child);
    const std::size_t last_separator = root.find_last_of("/\\");
    std::string upper = root;
    bool recased = false;
    for (std::size_t i = last_separator == std::string::npos
                             ? 0 : last_separator + 1;
         i < upper.size(); ++i) {
        if (upper[i] >= 'a' && upper[i] <= 'z') {
            upper[i] = static_cast<char>(upper[i] - 'a' + 'A');
            recased = true;
        }
    }
    if (!recased) {
        std::cout << "SKIP case sensitivity: nothing to recase\n";
        ++skips;
        return;
    }
    std::string relative;
    const bool mapped = easypb_file::strip_search_root_prefix(
        join_arg(upper, "f.proto"), root, relative);
    if (parent_probe == 1 || parent_probe == -2) {
        // Different cases name different directories here: the
        // recased spelling is outside the root instead of matching.
        CHECK(!mapped);
    } else {
        CHECK(mapped);
        CHECK(relative == "f.proto");
    }
    // Spelling equivalence follows the same rule for the file name:
    // hardlink-style case variants of one file deduplicate only where
    // the parent directory is not case-sensitive.
    const bool names_equivalent =
        easypb_file::equivalent_disk_spelling(join_arg(root, "F.proto"),
                                              join_arg(root, "f.proto"));
    if (child_probe == 1 || child_probe == -2) {
        CHECK(!names_equivalent);
    } else {
        CHECK(names_equivalent);
    }
}
#endif

#ifdef _WIN32
void test_wide_cli_conversion_is_unambiguous()
{
    // The old ACP/UTF-8 heuristic confused CP1252 bytes C3 A9 for the
    // filename "A�Ac" with UTF-8 C3 A9 for "Ac". Starting from wchar_t argv
    // makes the mapping deterministic: the two Unicode strings remain
    // distinct UTF-8 spellings before any loader/path code sees them.
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

void test_case_query_operational_error_propagates()
{
    // Exercise the exact state that used to be collapsed into an ordinary
    // prefix mismatch. No real filesystem ACL/case-sensitive directory is
    // required: the injected query returns the operational-error state.
    case_query_error_probe_calls = 0;
    easypb_file::set_directory_case_sensitivity_probe(
        forced_case_query_operational_error);

    std::string relative;
    bool operational_error = false;
    const bool mapped = easypb_file::strip_search_root_prefix(
        "C:/Temp/Root/file.proto", "C:/Temp/root", relative,
        &operational_error);

    easypb_file::set_directory_case_sensitivity_probe(0);
    CHECK(!mapped);
    CHECK(operational_error);
    CHECK(case_query_error_probe_calls == 1);
    CHECK(relative.empty());
}

void test_case_query_operational_error_reaches_loader()
{
    require_work_root();
    const std::string actual_dir = work_path("case-query-loader/CaseRoot");
    CHECK(write_file(join_arg(actual_dir, "root.proto"),
                     "syntax = \"proto2\";\nmessage Root {}\n"));

    std::string search_root = actual_dir;
    const std::string needle = "CaseRoot";
    const std::size_t pos = search_root.rfind(needle);
    CHECK(pos != std::string::npos);
    if (pos == std::string::npos) return;
    search_root.replace(pos, needle.size(), "caseroot");

    case_query_error_probe_calls = 0;
    easypb_file::set_directory_case_sensitivity_probe(
        forced_case_query_operational_error);

    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(search_root);
    std::vector<std::string> roots(1, join_arg(actual_dir, "root.proto"));
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    const bool ok =
        easypb_proto::load_source_files(options, roots, files, error, 0);

    easypb_file::set_directory_case_sensitivity_probe(0);
    CHECK(!ok);
    CHECK(case_query_error_probe_calls == 1);
    CHECK(error.message.find("cannot query case sensitivity") !=
          std::string::npos);
    CHECK(error.message.find(search_root) != std::string::npos);
    CHECK(files.files().empty());
    CHECK(files.targets().empty());
}

void test_confirm_identity_error_is_operational()
{
    // An identity failure during fold containment confirmation must
    // stay operational even when the existence probe still reports
    // the file as present: degrading into an isolated success would
    // lose the error. The identity seam fails the root-spelled
    // candidate only; real filesystem status answers the rest.
    require_work_root();
    const std::string actual_dir = work_path("confirm-probe/ConfirmRoot");
    CHECK(write_file(join_arg(actual_dir, "x.proto"),
                     "syntax = \"proto2\";\nmessage X {}\n"));

    std::string folded_dir = actual_dir;
    const std::string needle = "ConfirmRoot";
    const std::size_t pos = folded_dir.rfind(needle);
    CHECK(pos != std::string::npos);
    if (pos == std::string::npos) return;
    folded_dir.replace(pos, needle.size(), "confirmroot");
    const std::string argument = easypb_file::absolute_disk_path(
        join_arg(folded_dir, "x.proto"));

    confirm_error_marker = needle;
    confirm_error_probe_calls = 0;
    easypb_file::set_file_identity_probe(failing_confirm_identity_probe);

    easypb_proto::SourceLoaderOptions options;
    options.proto_paths.push_back(actual_dir);
    std::vector<std::string> roots(1, argument);
    easypb_schema::SchemaSet files;
    easypb_schema::Diagnostic error;
    const bool ok =
        easypb_proto::load_source_files(options, roots, files, error, 0);

    easypb_file::set_file_identity_probe(0);
    confirm_error_marker.clear();
    CHECK(!ok);
    CHECK(confirm_error_probe_calls == 1);
    CHECK(error.message.find("cannot confirm containment") !=
          std::string::npos);
    CHECK(files.files().empty());
    CHECK(files.targets().empty());
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
    CHECK(easypb_proto::load_source_files(options, roots, files, error, 0));
    CHECK(files.find_file("root.proto") != 0);
}

void test_historical_dbcs_trail_character_is_plain_unicode()
{
    // U+2015 historically encodes as CP932 81 5C, but no CP932 bytes ever
    // enter production now. The UTF-8 spelling must survive separator
    // normalization, joining and W-API file access as an ordinary character.
    require_work_root();
    const std::string mark = utf8_from_code_point(0x2015u);
    const std::string dir = work_path(std::string("trail-") + mark);
    const std::string file = join_arg(dir, "root.proto");
    CHECK(write_file(file, "syntax = \"proto2\";\nmessage Root {}\n"));
    const std::string absolute = easypb_file::lexical_absolute(dir + "/./");
    CHECK(easypb_file::disk_file_exists(
        easypb_file::join_disk_path(absolute, "root.proto")));
}

void test_case_query_error_classification()
{
    CHECK(easypb_file::is_directory_case_query_unsupported_error(
        ERROR_INVALID_PARAMETER));
    CHECK(easypb_file::is_directory_case_query_unsupported_error(
        ERROR_NOT_SUPPORTED));
    CHECK(easypb_file::is_directory_case_query_unsupported_error(
        ERROR_CALL_NOT_IMPLEMENTED));
    CHECK(!easypb_file::is_directory_case_query_unsupported_error(
        ERROR_ACCESS_DENIED));
    CHECK(!easypb_file::is_directory_case_query_unsupported_error(
        ERROR_IO_DEVICE));
}
#endif

#ifdef EASYPB_EXTENDED_TESTS
void test_extended_search_root_order_matrix()
{
    const std::string base = work_path("extended-root-order");
    const std::size_t root_count = 24;
    std::vector<std::string> paths;
    for (std::size_t i = 0; i < root_count; ++i) {
        std::ostringstream name;
        name << base << "/root-" << i;
        paths.push_back(name.str());
        std::ostringstream body;
        body << "syntax = \"proto2\";\nmessage Pick" << i << " {}\n";
        CHECK(write_file(join_arg(paths.back(), "root.proto"), body.str()));
    }

    // Rotate the ordered root list. The first entry must win every time,
    // even though every later root contains the same logical name.
    for (std::size_t rotation = 0; rotation < root_count; ++rotation) {
        easypb_proto::SourceLoaderOptions options;
        for (std::size_t offset = 0; offset < root_count; ++offset) {
            options.proto_paths.push_back(paths[(rotation + offset) % root_count]);
        }
        std::vector<std::string> roots(1, "root.proto");
        easypb_schema::SchemaSet files;
        easypb_schema::Diagnostic error;
        easypb_proto::LoadStatistics stats;
        CHECK(easypb_proto::load_source_files(options, roots, files, error, &stats));
        CHECK(files.files().size() == 1);
        CHECK(stats.files_read == 1);
        CHECK(stats.files_parsed == 1);
        easypb_schema::SchemaFile* root = files.find_file("root.proto");
        CHECK(root != 0);
        if (root != 0) {
            CHECK(root->file.message_type.size() == 1);
            if (root->file.message_type.size() == 1) {
                std::ostringstream expected;
                expected << "Pick" << rotation;
                CHECK(text(root->file.message_type[0].name) == expected.str());
            }
        }
    }
}

void test_extended_fault_injection_matrix()
{
    const std::string base = work_path("extended-fault-matrix");
    const std::size_t root_count = 24;
    std::vector<std::string> paths;
    for (std::size_t i = 0; i < root_count; ++i) {
        std::ostringstream name;
        name << base << "/fault-root-" << i;
        paths.push_back(name.str());
        CHECK(write_file(join_arg(paths.back(), "root.proto"),
                         "syntax = \"proto2\";\nmessage Root {}\n"));
    }

    for (std::size_t failing = 0; failing + 1 < root_count; ++failing) {
        easypb_proto::SourceLoaderOptions options;
        options.proto_paths = paths;
        std::vector<std::string> roots(1, "root.proto");
        easypb_schema::SchemaSet files;
        easypb_schema::Diagnostic error;

        extended_fault_marker = paths[failing];
        extended_file_marker = paths[failing + 1];
        extended_fault_probe_calls = 0;
        extended_file_probe_calls = 0;
        easypb_file::set_file_status_probe(extended_search_fault_probe);
        const bool ok = easypb_proto::load_source_files(options, roots, files, error, 0);
        easypb_file::set_file_status_probe(0);

        CHECK(!ok);
        CHECK(files.files().empty());
        CHECK(extended_fault_probe_calls == 1);
        CHECK(extended_file_probe_calls == 0);
        CHECK(error.message.find("synthetic extended search-root failure") !=
              std::string::npos);
    }

    extended_fault_marker.clear();
    extended_file_marker.clear();
}

void extended_concurrent_loader_worker(const std::string& dir,
                                       std::atomic<int>& errors)
{
    for (int iteration = 0; iteration < 100; ++iteration) {
        easypb_proto::SourceLoaderOptions options;
        options.proto_paths.push_back(dir);
        std::vector<std::string> roots(1, "root.proto");
        easypb_schema::SchemaSet files;
        easypb_schema::Diagnostic error;
        easypb_proto::LoadStatistics stats;
        if (!easypb_proto::load_source_files(options, roots, files, error, &stats) ||
            files.files().size() != 3 || stats.files_read != 3 ||
            stats.files_parsed != 3) {
            ++errors;
        }
    }
}

void test_extended_concurrent_read_only_loads()
{
    const std::string dir = work_path("extended-concurrency");
    CHECK(write_file(join_arg(dir, "root.proto"),
                     "syntax = \"proto2\";\n"
                     "import \"left.proto\";\n"
                     "import \"right.proto\";\n"
                     "message Root {}\n"));
    CHECK(write_file(join_arg(dir, "left.proto"),
                     "syntax = \"proto2\";\nmessage Left {}\n"));
    CHECK(write_file(join_arg(dir, "right.proto"),
                     "syntax = \"proto2\";\nmessage Right {}\n"));

    std::atomic<int> errors(0);
    std::vector<std::thread> workers;
    for (int i = 0; i < 8; ++i) {
        workers.push_back(std::thread(extended_concurrent_loader_worker,
                                      dir, std::ref(errors)));
    }
    for (std::size_t i = 0; i < workers.size(); ++i) workers[i].join();
    CHECK(errors.load() == 0);
}
#endif

void test_nested_duplicate_reports_root_chain()
{
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
    CHECK(!easypb_proto::load_source_files(options, roots, files, error, 0));
    CHECK(error.code == easypb_schema::DIAGNOSTIC_INVALID_IMPORT);
    CHECK(error.message.find("duplicate import") != std::string::npos);
    CHECK(error.message.find("\"root.proto\" -> \"child.proto\"") !=
          std::string::npos);
    CHECK(files.files().empty());
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2 || argv[1][0] == '\0') {
        std::cerr << "usage: test_loader <fixture-data-dir> [work-dir]\n"
                  << "When work-dir is omitted, ./imports-loader-work under\n"
                  << "the current working directory is used. Run via ctest\n"
                  << "so scratch stays under build/package-imports/; for\n"
                  << "manual runs pass an explicit work-dir under build/.\n";
        return EXIT_FAILURE;
    }
    fixture_dir = argv[1];
    if (argc >= 3 && argv[2][0] != '\0') work_root = argv[2];
    if (!file_exists(join_arg(fixture_dir, "diamond/root.proto"))) {
        std::cerr << "fixture directory lacks diamond/root.proto: "
                  << fixture_dir << '\n';
        return EXIT_FAILURE;
    }
    require_work_root();

    test_diamond_caching_and_counters();
    test_search_precedence_first_wins();
    test_search_precedence_error_stops_before_later_root();
    test_nested_operational_error_preserves_import_context();
    test_nul_byte_in_disk_path_fails_loudly();
    test_nul_byte_in_import_fails();
    test_case_fold_mismatch_is_not_containment();
    test_fold_hardlink_alias_names_fail();
    test_fold_hardlink_file_stays_isolated();
    test_shadowed_root_reports_error();
    test_disk_root_with_invalid_logical_name_fails();
    test_root_deduplication_preserves_order();
    test_missing_unused_import_fails();
    test_missing_weak_import_fails();
    test_missing_public_import_fails();
    test_invalid_syntax_preserves_chain();
    test_self_cycle_via_files();
    test_two_node_cycle_via_files();
    test_invalid_import_spelling_is_rejected();
    test_distinct_files_load_together();
    test_symlink_alias_detection();
    test_nested_alias_reports_importer_context();
    test_soft_hyphen_directories_stay_distinct();
    test_spaces_in_paths();
    test_non_ascii_filenames_round_trip();
    test_utf8_import_finds_unicode_file();
    test_empty_search_roots_mean_working_directory();
    test_windows_separators_in_disk_roots();
    test_case_alias_and_distinct_files();
    test_isolated_absolute_root();
    test_join_preserves_root_separator();
    test_strip_search_root_prefix();
#ifdef _WIN32
    test_join_preserves_bare_drive_semantics();
    test_non_ascii_case_alias_matches_search_root();
    test_case_sensitive_directory_semantics();
    test_wide_cli_conversion_is_unambiguous();
    test_case_query_operational_error_propagates();
    test_case_query_operational_error_reaches_loader();
    test_confirm_identity_error_is_operational();
    test_junction_fold_mismatch_is_not_containment();
    test_junction_fold_alias_names_fail();
    test_junction_same_entry_case_alias();
    test_unicode_search_root_uses_wide_filesystem_boundary();
    test_historical_dbcs_trail_character_is_plain_unicode();
    test_case_query_error_classification();
#endif
#ifdef _WIN32
    test_absolute_path_normalizes_partial_forms();
    test_drive_relative_root_is_not_isolated();
    test_separator_normalization_preserves_multibyte();
#endif
    test_utf8_logical_spelling_rules();
    test_isolated_root_with_resolvable_imports_fails();
    test_equivalent_isolated_roots_deduplicate();
    test_distinct_isolated_hardlink_names_fail();
    test_nested_cycle_reports_root_chain();
    test_nested_duplicate_reports_root_chain();
    test_failed_load_leaves_empty_result();
#ifdef EASYPB_EXTENDED_TESTS
    test_extended_search_root_order_matrix();
    test_extended_fault_injection_matrix();
    test_extended_concurrent_read_only_loads();
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
