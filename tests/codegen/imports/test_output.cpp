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
// half of P2 is injected here directly, because a denied directory (POSIX) or
// a read-only replacement target (Windows) is a publisher-level failure with
// an observable the CLI cannot stage portably.
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
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
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

std::vector<std::string> list_names(const std::string& directory)
{
    std::vector<std::string> names;
#ifdef _WIN32
    WIN32_FIND_DATAW entry;
    std::vector<wchar_t> wide_pattern;
    const std::string pattern = directory + "/*";
    if (!easypb_file::wide_win32_path(pattern, wide_pattern) ||
        wide_pattern.empty()) {
        return names;
    }
    wide_pattern.push_back(L'\0');
    HANDLE handle = FindFirstFileW(&wide_pattern[0], &entry);
    if (handle == INVALID_HANDLE_VALUE) return names;
    do {
        std::string name;
        if (!easypb_file::utf8_from_wide(entry.cFileName, name)) continue;
        if (name == "." || name == "..") continue;
        names.push_back(name);
    } while (FindNextFileW(handle, &entry));
    FindClose(handle);
#else
    DIR* dir = ::opendir(directory.c_str());
    if (dir == 0) return names;
    struct dirent* entry = 0;
    while ((entry = ::readdir(dir)) != 0) {
        const std::string name(entry->d_name);
        if (name == "." || name == "..") continue;
        names.push_back(name);
    }
    ::closedir(dir);
#endif
    return names;
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

    bool leftovers = false;
    CHECK(any_temporary_files(out, leftovers));
    CHECK(!leftovers);
    CHECK(any_temporary_files(path_join(out, "sub"), leftovers));
    CHECK(!leftovers);
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

void test_publisher_rejects_unsafe_names_before_writing(
    const std::string& work)
{
    const std::string out = path_join(work, "unsafe");
    CHECK(make_directory(out));

    std::vector<easypb_codegen::GeneratedFile> files;
    files.push_back(generated("good.pb.hpp", "GOOD"));
    files.push_back(generated("../escape", "EVIL"));

    easypb_schema::Diagnostic error;
    CHECK(!easypb_codegen::publish_headers(out, files, error));
    CHECK(error.message.find("not a safe output path") != std::string::npos);
    // The valid first entry must not have been written: all validation runs
    // before the first byte reaches the tree.
    CHECK(!path_exists(path_join(out, "good.pb.hpp")));
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
    CHECK(error.message.find("existing directory") != std::string::npos);
}

// --- P6: containment --------------------------------------------------------

void test_containment_is_component_exact_and_link_aware(
    const std::string& work)
{
    const std::string root = path_join(work, "contained");
    const std::string sibling = path_join(work, "contained-sibling");
    CHECK(make_directory(root));
    CHECK(make_directory(sibling));

    std::string error;
    // A sibling with a shared textual prefix is NOT inside the root.
    CHECK(!easypb_file::confirm_fold_containment(
        path_join(sibling, "x.pb.hpp"), root, error));
    CHECK(error.empty());

    // A path inside the root is contained even before it exists.
    error.clear();
    CHECK(easypb_file::confirm_fold_containment(
        path_join(root, "sub/x.pb.hpp"), root, error));
    CHECK(error.empty());

#ifndef _WIN32
    // A symlink strictly below the root redirects the write and must be
    // refused with a diagnostic, never silently accepted or reported as
    // "outside".
    const std::string outside = path_join(work, "outside");
    CHECK(make_directory(outside));
    const std::string link_path = path_join(root, "linked");
    (void)::remove(link_path.c_str());
    CHECK(::symlink(outside.c_str(), link_path.c_str()) == 0);
    error.clear();
    CHECK(!easypb_file::confirm_fold_containment(
        path_join(link_path, "x.pb.hpp"), root, error));
    CHECK(error.find("link") != std::string::npos);
#endif
}

// --- P7: native collisions --------------------------------------------------

bool create_hardlink(const std::string& existing, const std::string& link_path)
{
#ifdef _WIN32
    std::vector<wchar_t> wide_existing;
    std::vector<wchar_t> wide_link;
    if (!easypb_file::wide_win32_path(existing, wide_existing) ||
        wide_existing.empty() ||
        !easypb_file::wide_win32_path(link_path, wide_link) ||
        wide_link.empty()) {
        return false;
    }
    wide_existing.push_back(L'\0');
    wide_link.push_back(L'\0');
    return CreateHardLinkW(&wide_link[0], &wide_existing[0], 0) != 0;
#else
    return ::link(existing.c_str(), link_path.c_str()) == 0;
#endif
}

void test_publisher_refuses_one_physical_entry_twice(const std::string& work)
{
    const std::string out = path_join(work, "hardlink");
    CHECK(make_directory(out));

    // Both planned outputs already exist and are one physical entry; the
    // publisher must refuse before replacing either of them.
    CHECK(write_text_file(path_join(out, "first.pb.hpp"), "FIRST"));
    CHECK(create_hardlink(path_join(out, "first.pb.hpp"),
                          path_join(out, "second.pb.hpp")));

    std::vector<easypb_codegen::GeneratedFile> files;
    files.push_back(generated("first.pb.hpp", "NEW FIRST"));
    files.push_back(generated("second.pb.hpp", "NEW SECOND"));

    easypb_schema::Diagnostic error;
    CHECK(!easypb_codegen::publish_headers(out, files, error));
    CHECK(error.message.find("one physical entry") != std::string::npos);
    std::string content;
    CHECK(read_text_file(path_join(out, "first.pb.hpp"), content));
    CHECK(content == "FIRST");
}

void test_publisher_measures_case_folding(const std::string& work)
{
    const std::string out = path_join(work, "case-alias");
    CHECK(make_directory(out));

    bool case_sensitive = true;
    std::string query_error;
    CHECK(easypb_file::query_directory_case_sensitivity(out, case_sensitive,
                                                        query_error));

    std::vector<easypb_codegen::GeneratedFile> files;
    files.push_back(generated("Case.pb.hpp", "UPPER"));
    files.push_back(generated("case.pb.hpp", "lower"));

    easypb_schema::Diagnostic error;
    const bool published =
        easypb_codegen::publish_headers(out, files, error);
    if (case_sensitive) {
        CHECK(published);
        CHECK(path_is_file(path_join(out, "Case.pb.hpp")));
        CHECK(path_is_file(path_join(out, "case.pb.hpp")));
    } else {
        CHECK(!published);
        CHECK(error.message.find("one entry on this case-folding filesystem") !=
              std::string::npos);
    }
}

// --- P2: a later-file failure leaves earlier files replaced, no partial ------

void test_publisher_keeps_old_content_when_a_later_replace_fails(
    const std::string& work)
{
    const std::string out = path_join(work, "later-failure");
    CHECK(make_directory(out));
    const std::string first = path_join(out, "first.pb.hpp");
    const std::string second_dir = path_join(out, "sub");
    const std::string second = path_join(second_dir, "second.pb.hpp");
    CHECK(make_directory(second_dir));
    CHECK(write_text_file(second, "OLD SECOND"));

    std::vector<easypb_codegen::GeneratedFile> files;
    files.push_back(generated("first.pb.hpp", "NEW FIRST"));
    files.push_back(generated("sub/second.pb.hpp", "NEW SECOND"));

    // The injection makes the second replacement fail after the first file is
    // already in place: a read-only target on Windows, a read-only directory
    // on POSIX. Either way the promised observable is the same.
#ifdef _WIN32
    std::vector<wchar_t> wide_second;
    CHECK(easypb_file::wide_win32_path(second, wide_second) &&
          !wide_second.empty());
    wide_second.push_back(L'\0');
    CHECK(SetFileAttributesW(&wide_second[0], FILE_ATTRIBUTE_READONLY) != 0);
#else
    CHECK(::chmod(second_dir.c_str(), 0500) == 0);
#endif

    easypb_schema::Diagnostic error;
    const bool published = easypb_codegen::publish_headers(out, files, error);

#ifdef _WIN32
    SetFileAttributesW(&wide_second[0], FILE_ATTRIBUTE_NORMAL);
#else
    CHECK(::chmod(second_dir.c_str(), 0700) == 0);
#endif

    CHECK(!published);
    CHECK(!error.message.empty());

    std::string content;
    CHECK(read_text_file(first, content));
    CHECK(content == "NEW FIRST");          // per-file replacement happened
    CHECK(read_text_file(second, content));
    CHECK(content == "OLD SECOND");         // the failed file kept its bytes
    bool leftovers = false;
    CHECK(any_temporary_files(out, leftovers));
    CHECK(!leftovers);
    CHECK(any_temporary_files(second_dir, leftovers));
    CHECK(!leftovers);
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
    test_publisher_rejects_unsafe_names_before_writing(root);
    test_publisher_reports_a_file_as_out_dir(root);
    test_publisher_rejects_an_existing_directory_at_the_target(root);
    test_containment_is_component_exact_and_link_aware(root);
    test_publisher_refuses_one_physical_entry_twice(root);
    test_publisher_measures_case_folding(root);
    test_publisher_keeps_old_content_when_a_later_replace_fails(root);

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all output tests passed\n";
    return EXIT_SUCCESS;
}
