// Tests for the plan-5 output half: the pure logical mapping
// (`codegen/header_paths.*`) and the standalone publisher
// (`codegen/header_output.*`).
//
// This file links no parser and no CLI, so it runs in every profile
// (EASYPB_CODEGEN_WITH_PROTO_PARSER=OFF included). It links
// `easypb_header_output` for publication and `easypb_codegen_core` for the
// `GeneratedFile` type and `generated_header_path`.
//
// The invariants this suite witnesses (plan 5 process package,
// build/plan5-codegen-integration-process/invariants.md, group P):
//   P4  output paths are fully validated before any write
//   P5  duplicate mapped output names are rejected before writing
//   P6  containment is lexical plus native confirmation
//   P7  collision detection includes native identity, not textual equality
//   P8  directory creation reports failure as failure
//   P10 the logical mapping stays pure and platform-free
// P1/P2's CLI-level snapshots live in `codegen.imports.cli`; the per-file
// half of P2 is injected here directly, because a refused replacement target
// (a read-only file on Windows, a directory on POSIX) is a publisher-level
// failure with an observable the CLI cannot stage portably.
//
// Scratch lives in a per-process directory named after this process id below
// the pinned working directory, so a rerun never reads a previous run's tree
// and the source tree is never touched.

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

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
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "file_paths.hpp"
#include "generation.hpp"
#include "header_output.hpp"
#include "header_paths.hpp"

namespace {

int failures = 0;

void check(bool condition, const char* expression, const char* file, int line)
{
    if (!condition) {
        std::cerr << file << ':' << line << ": CHECK failed: " << expression
                  << '\n';
        ++failures;
    }
}

#define CHECK(x) check((x), #x, __FILE__, __LINE__)

std::string path_join(const std::string& a, const std::string& b)
{
    if (a.empty()) return b;
    return a + "/" + b;
}

unsigned long process_id()
{
#ifdef _WIN32
    return static_cast<unsigned long>(GetCurrentProcessId());
#else
    return static_cast<unsigned long>(::getpid());
#endif
}

bool write_text_file(const std::string& path, const std::string& contents)
{
    std::ofstream output(path.c_str(), std::ios::out | std::ios::binary);
    if (!output) return false;
    output.write(contents.data(), std::streamsize(contents.size()));
    output.close();
    return !output.fail();
}

bool read_text_file(const std::string& path, std::string& contents)
{
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input) return false;
    contents.assign(std::istreambuf_iterator<char>(input),
                    std::istreambuf_iterator<char>());
    return input.good() || input.eof();
}

bool path_exists(const std::string& path)
{
    std::string error;
    return easypb_file::disk_file_status(path, error) !=
           easypb_file::FILE_STATUS_ABSENT;
}

bool path_is_file(const std::string& path)
{
    std::string error;
    return easypb_file::disk_file_status(path, error) ==
           easypb_file::FILE_STATUS_FILE;
}

bool path_is_directory(const std::string& path)
{
    std::string error;
    return easypb_file::disk_file_status(path, error) ==
           easypb_file::FILE_STATUS_DIRECTORY;
}

bool make_directory(const std::string& path)
{
    std::string error;
    return easypb_file::make_directories(path, error);
}

// Every temp file the publisher may leave beside a target ends in this
// suffix; a clean publication leaves none of them anywhere.
bool any_temporary_files(const std::string& directory, bool& found)
{
    found = false;
#ifdef _WIN32
    WIN32_FIND_DATAW entry;
    std::vector<wchar_t> wide_pattern;
    const std::string pattern = directory + "/*";
    if (!easypb_file::wide_win32_path(pattern, wide_pattern) ||
        wide_pattern.empty()) {
        return false;
    }
    wide_pattern.push_back(L'\0');
    HANDLE handle = FindFirstFileW(&wide_pattern[0], &entry);
    if (handle == INVALID_HANDLE_VALUE) return true;  // empty directory
    do {
        std::string name;
        if (!easypb_file::utf8_from_wide(entry.cFileName, name)) continue;
        if (name == "." || name == "..") continue;
        if (name.find(".easypb-tmp") != std::string::npos) found = true;
    } while (FindNextFileW(handle, &entry));
    FindClose(handle);
    return true;
#else
    DIR* dir = ::opendir(directory.c_str());
    if (dir == 0) return false;
    struct dirent* entry = 0;
    while ((entry = ::readdir(dir)) != 0) {
        const std::string name(entry->d_name);
        if (name == "." || name == "..") continue;
        if (name.find(".easypb-tmp") != std::string::npos) found = true;
    }
    ::closedir(dir);
    return true;
#endif
}

easypb_codegen::GeneratedFile generated(const std::string& name,
                                        const std::string& content)
{
    easypb_codegen::GeneratedFile file;
    file.name = name;
    file.content = content;
    return file;
}

// --- P10: the pure logical mapping ------------------------------------------

void test_generated_header_path_maps_and_rejects()
{
    CHECK(easypb_codegen::generated_header_path("a.proto") == "a.pb.hpp");
    CHECK(easypb_codegen::generated_header_path("dir/a.proto") ==
          "dir/a.pb.hpp");
    CHECK(easypb_codegen::generated_header_path("a/b/c") == "a/b/c.pb.hpp");
    CHECK(easypb_codegen::generated_header_path("a.protobuf") ==
          "a.protobuf.pb.hpp");
    // W-25: a final `.protodevel` is stripped first, exactly like the
    // reference compiler (compiler/code_generator_lite.cc:41-48); only the
    // final suffix is removed, and `.protodevel` beats `.proto`.
    CHECK(easypb_codegen::generated_header_path("legacy.protodevel") ==
          "legacy.pb.hpp");
    CHECK(easypb_codegen::generated_header_path("dir/legacy.protodevel") ==
          "dir/legacy.pb.hpp");
    CHECK(easypb_codegen::generated_header_path("legacy.protodevel.proto") ==
          "legacy.protodevel.pb.hpp");
    CHECK(easypb_codegen::generated_header_path("legacy.proto.protodevel") ==
          "legacy.proto.pb.hpp");
    // Only the FINAL suffix is a `.proto` replacement, and a dot-leading
    // component is still a component.
    CHECK(easypb_codegen::generated_header_path("x.proto/y") ==
          "x.proto/y.pb.hpp");

    const char* rejected[] = {
        "",
        "../escape.proto",
        "/absolute.proto",
        "C:/drive.proto",
        "back\\slash.proto",
        "quote\"d.proto",
        "control\n.proto",
        "dir//empty.proto",
        "dir/./dot.proto",
    };
    for (std::size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        bool threw = false;
        try {
            easypb_codegen::generated_header_path(rejected[i]);
        } catch (const std::exception&) {
            threw = true;
        }
        if (!threw) {
            std::cerr << "generated_header_path accepted \"" << rejected[i]
                      << "\"\n";
            ++failures;
        }
    }
}

// --- P4/P5/P8: validation before any write ----------------------------------

void test_publisher_writes_and_replaces(const std::string& work)
{
    const std::string out = path_join(work, "happy");
    CHECK(make_directory(out));
    const std::string existing = path_join(out, "old.pb.hpp");
    CHECK(write_text_file(existing, "OLD CONTENT"));

    // The publisher receives names that `generated_header_path` already
    // mapped (that is what `GeneratedFile::name` means); it never maps twice.
    std::vector<easypb_codegen::GeneratedFile> files;
    files.push_back(generated("sub/new.pb.hpp", "NEW ONE\n"));
    files.push_back(generated("old.pb.hpp", "REPLACED\n"));

    easypb_schema::Diagnostic error;
    CHECK(easypb_codegen::publish_headers(out, files, error));
    CHECK(error.message.empty());

    std::string content;
    CHECK(read_text_file(path_join(out, "sub/new.pb.hpp"), content));
    CHECK(content == "NEW ONE\n");
    CHECK(read_text_file(existing, content));
    CHECK(content == "REPLACED\n");

}

void test_publisher_rejects_duplicate_names_before_writing(
    const std::string& work)
{
    const std::string out = path_join(work, "duplicates");
    CHECK(make_directory(out));

    std::vector<easypb_codegen::GeneratedFile> files;
    files.push_back(generated("one.pb.hpp", "A"));
    files.push_back(generated("one.pb.hpp", "B"));

    easypb_schema::Diagnostic error;
    CHECK(!easypb_codegen::publish_headers(out, files, error));
    CHECK(error.message.find("two targets map to one output file") !=
          std::string::npos);
    CHECK(!path_exists(path_join(out, "one.pb.hpp")));
}

// The publisher refuses a name that escapes the output root.
//
// The reference compiler writes such a name: protoc writes one directory
// ABOVE --cpp_out when a descriptor set asks it to (measured against protoc
// 30.1, build/plan5-code-review-1021-summary/verify/G2.md). This project
// refuses it, by the owner's decision of 2026-10-21, and the refusal happens
// in the publisher itself: the name a GeneratedFile carries is a logical path
// (the contract codegen/header_paths.hpp states), and a name that is not one
// never becomes a file.
//
// The mapping layer refuses the same spelling one layer up, so the check here
// closes the hole a hand-built GeneratedFile would otherwise have.
void test_publisher_refuses_a_name_that_escapes_the_root(
    const std::string& work)
{
    const std::string out = path_join(work, "escaping");
    CHECK(make_directory(out));

    // The upstream refusal is the one that holds.
    bool threw = false;
    try {
        (void)easypb_codegen::generated_header_path("../escape.proto");
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);

    // And the publisher refuses it too, with a diagnostic, and writes
    // nothing.
    std::vector<easypb_codegen::GeneratedFile> files;
    files.push_back(generated("../escape.pb.hpp", "ESCAPE"));
    easypb_schema::Diagnostic error;
    CHECK(!easypb_codegen::publish_headers(out, files, error));
    CHECK(!error.message.empty());
    CHECK(error.message.find("cannot be published") != std::string::npos);
    CHECK(!path_exists(path_join(work, "escape.pb.hpp")));

    // The same for a name that escapes through a nested component.
    std::vector<easypb_codegen::GeneratedFile> nested;
    nested.push_back(generated("sub/../../escape2.pb.hpp", "ESCAPE2"));
    easypb_schema::Diagnostic nested_error;
    CHECK(!easypb_codegen::publish_headers(out, nested, nested_error));
    CHECK(!nested_error.message.empty());
    CHECK(!path_exists(path_join(work, "escape2.pb.hpp")));

    // A name that stays inside the root is still published.
    std::vector<easypb_codegen::GeneratedFile> good;
    good.push_back(generated("sub/inside.pb.hpp", "INSIDE"));
    easypb_schema::Diagnostic good_error;
    CHECK(easypb_codegen::publish_headers(out, good, good_error));
    CHECK(good_error.message.empty());
    CHECK(path_is_file(path_join(out, "sub/inside.pb.hpp")));
}

// The directory creation is bounded.
//
// The recursion this replaces walked one level per path component with no
// bound, and a descriptor set whose logical name is a deep chain of
// components drove it into a stack overflow (0xC00000FD). The chain is
// created iteratively now, and a path with more components than the limit is
// refused with a diagnostic - before the first directory is created, so a
// refused path leaves no tree behind.
void test_directory_creation_is_bounded(const std::string& work)
{
    // A shallow path is created.
    const std::string shallow = path_join(work, "shallow");
    std::string error;
    CHECK(easypb_file::make_directories(path_join(shallow, "a/b/c"), error));
    CHECK(error.empty());
    CHECK(path_is_directory(path_join(shallow, "a/b/c")));

    // A path with more components than the limit is refused, and nothing is
    // created. The path is built in memory only: the tool must not build a
    // tree of that depth to find out.
    std::string deep = path_join(work, "deep");
    for (std::size_t i = 0; i < 200; ++i) deep += "/d";
    std::string deep_error;
    CHECK(!easypb_file::make_directories(deep, deep_error));
    CHECK(!deep_error.empty());
    CHECK(deep_error.find("components") != std::string::npos);
    CHECK(!path_exists(path_join(work, "deep")));
}

void test_publisher_reports_a_file_as_out_dir(const std::string& work)
{
    const std::string out = path_join(work, "file-as-root");
    CHECK(write_text_file(out, "I AM A FILE"));

    std::vector<easypb_codegen::GeneratedFile> files;
    files.push_back(generated("x.pb.hpp", "X"));

    easypb_schema::Diagnostic error;
    CHECK(!easypb_codegen::publish_headers(out, files, error));
    CHECK(!error.message.empty());
    std::string content;
    CHECK(read_text_file(out, content));
    CHECK(content == "I AM A FILE");
}

// CR-40: when a file sits in the MIDDLE of the requested output path, the
// diagnostic names that component. The requested path does not exist, so
// calling it "an existing file" sends the reader to remove the wrong entry.
// The component travels separately from the reason: the reason has no path
// (the action-entry rule) and the caller labels the diagnostic with the first
// component that answered.
void test_blocking_component_is_reported(const std::string& work)
{
    const std::string blocker = path_join(work, "component-blocker.bin");
    CHECK(write_text_file(blocker, "I AM A FILE"));

    std::string error;
    std::string blocking;
    CHECK(!easypb_file::make_directories(blocker + "/a/b", error, &blocking));
    CHECK(error == "output path is an existing file, not a directory");
    CHECK(blocking == easypb_file::lexical_absolute(blocker));

    // The requested directory itself is the blocker: nothing above it is news
    // to the caller, so the label stays the spelling the caller passed.
    std::string direct_error;
    std::string direct_blocking;
    CHECK(!easypb_file::make_directories(blocker, direct_error,
                                         &direct_blocking));
    CHECK(direct_error == "output path is an existing file, not a directory");
    CHECK(direct_blocking.empty());

    // The publisher's second directory call: the output root exists and the
    // parent of the mapped header is the file.
    const std::string out = path_join(work, "component-out");
    CHECK(make_directory(out));
    const std::string parent_blocker = path_join(out, "api");
    CHECK(write_text_file(parent_blocker, "I AM A FILE"));
    std::vector<easypb_codegen::GeneratedFile> files;
    files.push_back(generated("api/x.pb.hpp", "X"));
    easypb_schema::Diagnostic publisher_error;
    CHECK(!easypb_codegen::publish_headers(out, files, publisher_error));
    CHECK(publisher_error.file ==
          easypb_file::lexical_absolute(parent_blocker));
    CHECK(publisher_error.message ==
          "output path is an existing file, not a directory");
    CHECK(!path_exists(path_join(out, "api/x.pb.hpp")));
}

void test_publisher_rejects_an_existing_directory_at_the_target(
    const std::string& work)
{
    const std::string out = path_join(work, "target-directory");
    CHECK(make_directory(out));
    CHECK(make_directory(path_join(out, "blocked.pb.hpp")));

    std::vector<easypb_codegen::GeneratedFile> files;
    files.push_back(generated("blocked.pb.hpp", "X"));

    easypb_schema::Diagnostic error;
    CHECK(!easypb_codegen::publish_headers(out, files, error));
    // W-29: the condition is named, and the reason carries no path: the
    // publisher's Diagnostic names the target itself.
    CHECK(error.message == "cannot write file (the target is a directory)");
    CHECK(error.message.find(out) == std::string::npos);
    CHECK(error.file == easypb_file::lexical_absolute(
                            path_join(out, "blocked.pb.hpp")));
    CHECK(path_is_directory(path_join(out, "blocked.pb.hpp")));
}

// W-29: a directory at the write target is its own failure on both platforms
// (Win32 answers ERROR_ACCESS_DENIED, POSIX EISDIR), and the action entry
// returns the reason alone; the caller names the path.
void test_write_reports_a_directory_target(const std::string& work)
{
    const std::string target = path_join(work, "write-directory.pb.hpp");
    CHECK(make_directory(target));

    std::string error;
    CHECK(!easypb_file::write_disk_file(target, "X", error));
    CHECK(error == "cannot write file (the target is a directory)");
    CHECK(error.find(target) == std::string::npos);
    CHECK(path_is_directory(target));
}

#ifdef _WIN32
// --- CR-31: the failed create's own code ------------------------------------
//
// The writer used to read GetLastError() after the directory probe, so the
// probe's code (ERROR_FILE_NOT_FOUND, 2) replaced the create's
// (ERROR_ACCESS_DENIED, 5). The injection makes the two differ: a fresh file in
// a directory whose DACL denies creation. A volume that cannot store a DACL
// (FAT/exFAT, some network shares) skips the row: the rule is about the writer,
// not about the filesystem.
bool apply_everyone_entry(const std::string& path, ACCESS_MODE access_mode,
                          DWORD permissions, DWORD inheritance,
                          SECURITY_INFORMATION security_information)
{
    std::vector<wchar_t> wide;
    if (!easypb_file::wide_win32_path(path, wide) || wide.empty()) return false;
    wide.push_back(L'\0');

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
    const DWORD result = SetNamedSecurityInfoW(&wide[0], SE_FILE_OBJECT,
                                               security_information, 0, 0, acl,
                                               0);
    LocalFree(acl);
    return result == ERROR_SUCCESS;
}

bool deny_create_files(const std::string& path)
{
    // PROTECTED drops the inherited allowances, so the deny is the whole DACL;
    // ownership still grants WRITE_DAC, which is what lets the caller restore
    // access below.
    return apply_everyone_entry(
        path, DENY_ACCESS, FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY,
        NO_INHERITANCE,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION);
}

bool allow_full_access(const std::string& path)
{
    return apply_everyone_entry(path, GRANT_ACCESS, GENERIC_ALL,
                                SUB_CONTAINERS_AND_OBJECTS_INHERIT,
                                DACL_SECURITY_INFORMATION |
                                    UNPROTECTED_DACL_SECURITY_INFORMATION);
}

void test_write_reports_the_create_error_code(const std::string& work)
{
    const std::string dir = path_join(work, "denied-create");
    CHECK(make_directory(dir));
    if (!deny_create_files(dir)) {
        std::cout
            << "skip: the volume cannot store a DACL (denied-create)\n";
        return;
    }

    const std::string target = path_join(dir, "new.pb.hpp");
    std::string error;
    const bool written = easypb_file::write_disk_file(target, "X", error);
    allow_full_access(dir);

    CHECK(!written);
    CHECK(error == "cannot write file (Win32 error 5)");
    CHECK(!path_exists(target));
}

// --- CR-32: a server-only network root creates nothing ----------------------
//
// "//server" names a server, not a directory; folding it into the rooted
// "/server" made the pre-fix writer create a directory at the drive root and
// fail only at the write. Nothing may be created, and the refusal names the
// real cause. The server name carries the process id so a rerun never reads a
// previous run's stray directory as its own.
void test_server_only_network_root_is_refused(const std::string& work)
{
    std::ostringstream name;
    name << "easycodegen-unc-" << process_id();
    const std::string server = name.str();
    const std::string absolute = easypb_file::lexical_absolute(work);
    CHECK(!absolute.empty());
    if (absolute.size() < 2) return;
    const std::string stray = absolute.substr(0, 2) + "/" + server;
    CHECK(!path_exists(stray));

    std::string error;
    CHECK(!easypb_file::make_directories("//" + server, error));
    CHECK(error ==
          "cannot create directory (a network path names a server but no share)");
    CHECK(!path_exists(stray));

    // The separator-only spelling and a trailing separator are the same shape.
    CHECK(!easypb_file::make_directories("//", error));
    CHECK(!easypb_file::make_directories("//" + server + "/", error));
    CHECK(!path_exists(stray));

    // V-33: the long-path spelling of a share-less network path is the same
    // condition. The translator has no plain spelling for an empty share, so
    // it must not fold it into "//server/"; the refusal names the cause.
    error.clear();
    CHECK(!easypb_file::make_directories(
        "\\\\?\\UNC\\" + server + "\\", error));
    CHECK(error ==
          "cannot create directory (a network path names a server but no share)");
    CHECK(!path_exists(stray));
    error.clear();
    CHECK(!easypb_file::make_directories(
        "//?/UNC/" + server + "/", error));
    CHECK(error ==
          "cannot create directory (a network path names a server but no share)");
    CHECK(!path_exists(stray));

    // A separator where the share component should begin is the same empty
    // share: before the pre-resolution check the spelling fell through to
    // lexical_absolute as "//?/UNC/server/nested" and named a directory
    // called "?" under the current root.
    error.clear();
    CHECK(!easypb_file::make_directories(
        "\\\\?\\UNC\\" + server + "\\\\nested", error));
    CHECK(error ==
          "cannot create directory (a network path names a server but no share)");
    CHECK(!path_exists(stray));

    // A server at the very end of the spelling names no share either.
    error.clear();
    CHECK(!easypb_file::make_directories(
        "\\\\?\\UNC\\" + server, error));
    CHECK(error ==
          "cannot create directory (a network path names a server but no share)");
    CHECK(!path_exists(stray));

    // A share root is a directory the network owns: the caller may name it,
    // nothing below the server is created here, and the write is where a
    // missing share fails.
    error.clear();
    CHECK(easypb_file::make_directories("//" + server + "/share", error));
    CHECK(!path_exists(stray));

    // The long-path spelling of a named share keeps working (V-33).
    error.clear();
    CHECK(easypb_file::make_directories(
        "\\\\?\\UNC\\" + server + "\\share", error));
    CHECK(error.empty());
    CHECK(!path_exists(stray));
}
#endif

// --- P6: containment --------------------------------------------------------
// Removed with the plan-5 trim (`fbdcc0e`, 2026-10-11): protoc writes wherever
// the mapped name
// points, including one directory above the output root when a descriptor set
// asks it to (measured). The name itself is still validated, upstream, by
// easypb_codegen::generated_header_path - test_generated_header_path_maps_and_
// rejects covers that, and it is the check that refuses "../escape".

// --- P7: native collisions --------------------------------------------------
//
// Removed with the same trim. protoc keeps its outputs in a map, so two targets
// mapping to one path silently write one file there; ours refuses the collision
// by name below (P5) and otherwise behaves like the reference.




// --- P2: a later-file failure leaves earlier files replaced, no partial ------
//
// The publisher writes one file at a time, in byte-wise name order, and stops
// at the first failure, reporting it as a diagnostic. The files it wrote
// before the failure stay written: there is no rollback, and the failure is
// never hidden.
//
// The refusal is staged where it needs no privilege. On Windows a read-only
// target does it. On POSIX a read-only target would do it too - but only for
// an ordinary user, and the container jobs run as root, which ignores the
// permission bits: the publication SUCCEEDED there and this test failed on
// every POSIX lane (measured: the matrix of 2026-10-22). A read-only
// *directory*, the injection this test used before, does not refuse the
// write at all: the publisher truncates the target in place, the way the
// reference compiler does, and truncating a file needs no permission on the
// directory that holds it.
//
// So on POSIX the target is a directory, which no user can open for writing.
// The write fails where the file would have been opened, so the target keeps
// what it held, and the publisher reports the failure.

void test_publisher_keeps_old_content_when_a_later_replace_fails(
    const std::string& work)
{
    const std::string out = path_join(work, "later-failure");
    CHECK(make_directory(out));
    const std::string first = path_join(out, "first.pb.hpp");
    const std::string second_dir = path_join(out, "sub");
    const std::string second = path_join(second_dir, "second.pb.hpp");
    CHECK(make_directory(second_dir));

    // The target of the second file, and the "old content" it holds.
#ifdef _WIN32
    CHECK(write_text_file(second, "OLD SECOND"));
    std::vector<wchar_t> wide_second;
    CHECK(easypb_file::wide_win32_path(second, wide_second) &&
          !wide_second.empty());
    wide_second.push_back(L'\0');
    CHECK(SetFileAttributesW(&wide_second[0], FILE_ATTRIBUTE_READONLY) != 0);
#else
    CHECK(make_directory(second));
    const std::string marker = path_join(second, "marker.txt");
    CHECK(write_text_file(marker, "OLD SECOND"));
#endif

    std::vector<easypb_codegen::GeneratedFile> files;
    files.push_back(generated("first.pb.hpp", "NEW FIRST"));
    files.push_back(generated("sub/second.pb.hpp", "NEW SECOND"));
    // A third file, written after the failing one: the publisher stops at the
    // first failure, so this one is never written.
    files.push_back(generated("third.pb.hpp", "NEW THIRD"));

    easypb_schema::Diagnostic error;
    const bool published = easypb_codegen::publish_headers(out, files, error);

#ifdef _WIN32
    SetFileAttributesW(&wide_second[0], FILE_ATTRIBUTE_NORMAL);
#endif

    CHECK(!published);
    CHECK(!error.message.empty());

    std::string content;
    CHECK(read_text_file(first, content));
    CHECK(content == "NEW FIRST");          // per-file replacement happened
#ifdef _WIN32
    CHECK(read_text_file(second, content));
#else
    CHECK(read_text_file(marker, content));
#endif
    CHECK(content == "OLD SECOND");         // the failed file kept its bytes
    CHECK(!path_exists(path_join(out, "third.pb.hpp")));  // it stopped there
    bool leftovers = false;
    CHECK(any_temporary_files(out, leftovers));
    CHECK(!leftovers);
    CHECK(any_temporary_files(second_dir, leftovers));
    CHECK(!leftovers);
}

// --- W-28: the writer's share mode -----------------------------------------

#ifdef _WIN32
HANDLE open_holder(const std::string& path, DWORD access, DWORD share)
{
    std::vector<wchar_t> wide;
    if (!easypb_file::wide_win32_path(path, wide) || wide.empty()) {
        return INVALID_HANDLE_VALUE;
    }
    wide.push_back(L'\0');
    return CreateFileW(&wide[0], access, share, 0, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, 0);
}

std::string backslash_spelling(const std::string& path)
{
    std::string result = path;
    for (std::size_t i = 0; i < result.size(); ++i) {
        if (result[i] == '/') result[i] = '\\';
    }
    return result;
}

std::string slash_spelling(const std::string& path)
{
    std::string result = path;
    for (std::size_t i = 0; i < result.size(); ++i) {
        if (result[i] == '\\') result[i] = '/';
    }
    return result;
}

// The canonical prefixed form of an absolute path: a drive spelling takes
// "\\?\", a network spelling takes "\\?\UNC\". The work directory can be on
// a share, so the test must not assume a drive.
std::string prefixed_spelling(const std::string& absolute)
{
    if (absolute.size() >= 2 && absolute[0] == '/' && absolute[1] == '/') {
        return "\\\\?\\UNC\\" + backslash_spelling(absolute.substr(2));
    }
    return "\\\\?\\" + backslash_spelling(absolute);
}
#endif

// The Windows writer must not deny a concurrent reader that permits a writer,
// nor a holder that already has write access. The share set is the measured
// one of the reference's _wopen (_SH_DENYNO): read and write, not delete. A
// reader that denies writers still blocks the open, in both tools.
void test_writer_shares_a_concurrent_reader(const std::string& work)
{
#ifdef _WIN32
    const std::string out = path_join(work, "share-mode");
    CHECK(make_directory(out));
    const std::string target = path_join(out, "shared.pb.hpp");
    std::string content;
    std::string error;

    // A reader that permits a writer.
    CHECK(write_text_file(target, "OLD"));
    HANDLE reader = open_holder(target, GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE);
    CHECK(reader != INVALID_HANDLE_VALUE);
    CHECK(easypb_file::write_disk_file(target, "NEW", error));
    CHECK(error.empty());
    if (reader != INVALID_HANDLE_VALUE) CloseHandle(reader);
    CHECK(read_text_file(target, content));
    CHECK(content == "NEW");

    // An existing holder with write access: this is the case that separates
    // FILE_SHARE_READ from FILE_SHARE_READ | FILE_SHARE_WRITE.
    CHECK(write_text_file(target, "OLD2"));
    HANDLE writer = open_holder(target, GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE);
    CHECK(writer != INVALID_HANDLE_VALUE);
    error.clear();
    CHECK(easypb_file::write_disk_file(target, "NEW2", error));
    CHECK(error.empty());
    if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
    CHECK(read_text_file(target, content));
    CHECK(content == "NEW2");

    // The control: a reader that denies writers. protoc fails here too, and
    // the bytes already on disk must survive.
    CHECK(write_text_file(target, "OLD3"));
    HANDLE exclusive = open_holder(target, GENERIC_READ, FILE_SHARE_READ);
    CHECK(exclusive != INVALID_HANDLE_VALUE);
    error.clear();
    CHECK(!easypb_file::write_disk_file(target, "NEW3", error));
    CHECK(error.find("cannot write file") != std::string::npos);
    if (exclusive != INVALID_HANDLE_VALUE) CloseHandle(exclusive);
    CHECK(read_text_file(target, content));
    CHECK(content == "OLD3");
#else
    (void)work;
#endif
}

// --- W-31: a relative output root with an unknown working directory ---------

// The natural trigger on POSIX is a deleted working directory: the process
// keeps running, but getcwd no longer has an answer. The child must refuse
// the relative spelling instead of anchoring it at the filesystem root.
void test_relative_output_uses_a_known_cwd(const std::string& work)
{
#ifndef _WIN32
    const std::string doomed = path_join(work, "deleted-cwd");
    CHECK(make_directory(doomed));
    const std::string absolute = easypb_file::lexical_absolute(doomed);
    CHECK(!absolute.empty());

    std::ostringstream relative_name;
    relative_name << "s7b-relative-output-" << process_id();
    const std::string name = relative_name.str();

    // Do not duplicate buffered output into the child.
    std::cout.flush();
    std::cerr.flush();
    const pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        if (::chdir(doomed.c_str()) != 0) ::_exit(10);
        if (::rmdir(absolute.c_str()) != 0) ::_exit(11);

        std::string error;
        if (easypb_file::make_directories(name, error)) ::_exit(12);
        if (error.find("current working directory") == std::string::npos) {
            ::_exit(13);
        }
        std::string status_error;
        if (easypb_file::disk_file_status("/" + name, status_error) !=
            easypb_file::FILE_STATUS_ABSENT) {
            ::_exit(14);
        }
        ::_exit(0);
    }

    int status = 0;
    CHECK(::waitpid(child, &status, 0) == child);
    const bool exited = WIFEXITED(status) != 0;
    if (!exited || WEXITSTATUS(status) != 0) {
        std::cerr << "deleted-cwd child failed with status " << status << '\n';
    }
    CHECK(exited);
    CHECK(exited && WEXITSTATUS(status) == 0);
    // The parent's working directory is untouched, and the scratch directory
    // the child removed stays removed.
    CHECK(!path_exists(doomed));
#else
    (void)work;
#endif
}

// --- W-34: an embedded NUL is refused at every write/create entry -----------

void test_embedded_nul_is_refused_on_write(const std::string& work)
{
    const std::string out = path_join(work, "nul-write");
    CHECK(make_directory(out));
    const std::string prefix = path_join(out, "nul-target");
    const std::string with_nul =
        prefix + std::string(1, '\0') + ".pb.hpp";

    std::string error;
    CHECK(!easypb_file::write_disk_file(with_nul, "CONTENT", error));
    CHECK(error.find("path contains NUL character") != std::string::npos);
    CHECK(!path_exists(prefix));

    error.clear();
    CHECK(!easypb_file::make_directories(with_nul, error));
    CHECK(error.find("path contains NUL character") != std::string::npos);
    CHECK(!path_exists(prefix));

    // The publisher reaches the same shared entry through its out-dir.
    std::vector<easypb_codegen::GeneratedFile> files;
    files.push_back(generated("nul-target.pb.hpp", "CONTENT"));
    easypb_schema::Diagnostic publish_error;
    CHECK(!easypb_codegen::publish_headers(with_nul, files, publish_error));
    CHECK(publish_error.message.find("path contains NUL character") !=
          std::string::npos);
    CHECK(!path_exists(prefix));
}

// --- W-46: the long-path prefix is a transport spelling ---------------------

void test_long_path_prefix_is_translated(const std::string& work)
{
#ifdef _WIN32
    // The two prefix spellings and the two bodies the module accepts.
    const std::string plain_drive = "C:/s7b-prefix/a/b";
    CHECK(easypb_file::lexical_absolute("\\\\?\\C:\\s7b-prefix\\a\\b") ==
          easypb_file::lexical_absolute(plain_drive));
    CHECK(easypb_file::lexical_absolute("//?/C:/s7b-prefix/a/b") ==
          easypb_file::lexical_absolute(plain_drive));
    CHECK(easypb_file::lexical_absolute(
              "\\\\?\\UNC\\server\\share\\x") ==
          easypb_file::lexical_absolute("//server/share/x"));
    CHECK(easypb_file::lexical_absolute("//?/UNC/server/share/x") ==
          easypb_file::lexical_absolute("//server/share/x"));

    const std::string out = path_join(work, "long-prefix");
    CHECK(make_directory(out));
    const std::string absolute_out = easypb_file::lexical_absolute(out);
    CHECK(!absolute_out.empty());
    const std::string prefixed_out = prefixed_spelling(absolute_out);
    const std::string forward_out = slash_spelling(prefixed_out);

    std::string error;
    const std::string target_plain =
        path_join(absolute_out, "prefix.pb.hpp");
    CHECK(easypb_file::write_disk_file(
        prefixed_out + "\\prefix.pb.hpp", "PREFIXED", error));
    CHECK(error.empty());
    std::string content;
    CHECK(read_text_file(target_plain, content));
    CHECK(content == "PREFIXED");

    // The forward-slash spelling of the same prefix.
    error.clear();
    CHECK(easypb_file::write_disk_file(
        forward_out + "/forward.pb.hpp", "FORWARD", error));
    CHECK(error.empty());
    CHECK(read_text_file(path_join(absolute_out, "forward.pb.hpp"),
                         content));
    CHECK(content == "FORWARD");

    // Directory creation sees the same spelling.
    error.clear();
    CHECK(easypb_file::make_directories(
        prefixed_out + "\\nested\\tree", error));
    CHECK(error.empty());
    CHECK(path_is_directory(path_join(out, "nested/tree")));

    // The publisher accepts the prefixed out-dir end to end.
    std::vector<easypb_codegen::GeneratedFile> files;
    files.push_back(generated("published.pb.hpp", "PUBLISHED"));
    easypb_schema::Diagnostic publish_error;
    CHECK(easypb_codegen::publish_headers(prefixed_out, files,
                                          publish_error));
    CHECK(publish_error.message.empty());
    CHECK(read_text_file(path_join(absolute_out, "published.pb.hpp"),
                         content));
    CHECK(content == "PUBLISHED");
#else
    (void)work;
#endif
}

} // namespace

int main()
{
    // One scratch tree per process, below the pinned working directory, so a
    // rerun never reads a previous run's state.
    std::ostringstream work_name;
    work_name << "codegen-output-work-" << process_id();
    const std::string root = work_name.str();
    CHECK(make_directory(root));

    test_generated_header_path_maps_and_rejects();
    test_publisher_writes_and_replaces(root);
    test_publisher_rejects_duplicate_names_before_writing(root);
    test_publisher_refuses_a_name_that_escapes_the_root(root);
    test_directory_creation_is_bounded(root);
    test_publisher_reports_a_file_as_out_dir(root);
    test_blocking_component_is_reported(root);
    test_publisher_rejects_an_existing_directory_at_the_target(root);
    test_write_reports_a_directory_target(root);
#ifdef _WIN32
    test_write_reports_the_create_error_code(root);
    test_server_only_network_root_is_refused(root);
#endif
    test_publisher_keeps_old_content_when_a_later_replace_fails(root);
    test_writer_shares_a_concurrent_reader(root);
    test_relative_output_uses_a_known_cwd(root);
    test_embedded_nul_is_refused_on_write(root);
    test_long_path_prefix_is_translated(root);

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all output tests passed\n";
    return EXIT_SUCCESS;
}
