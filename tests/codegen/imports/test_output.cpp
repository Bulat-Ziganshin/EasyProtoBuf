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

// The publisher writes the name it is handed, the way the reference compiler
// does. A name that escapes the output root is refused one level earlier, where
// the generator maps it: generated_header_path (the pure logical mapping)
// throws for a logical name carrying "..", so that is the check a descriptor set
// naming "../escape.proto" meets. Measured against protoc 30.1 on exactly that
// input: protoc writes one directory ABOVE the output directory, rc=0.
void test_unsafe_names_are_refused_by_the_mapping_not_the_publisher(
    const std::string& work)
{
    const std::string out = path_join(work, "unsafe");
    CHECK(make_directory(out));

    // The upstream refusal is the one that holds.
    bool threw = false;
    try {
        (void)easypb_codegen::generated_header_path("../escape.proto");
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);

    // And a hand-built list still publishes what it is given: the publisher no
    // longer re-validates, exactly like the reference.
    std::vector<easypb_codegen::GeneratedFile> files;
    files.push_back(generated("good.pb.hpp", "GOOD"));

    easypb_schema::Diagnostic error;
    CHECK(easypb_codegen::publish_headers(out, files, error));
    CHECK(path_is_file(path_join(out, "good.pb.hpp")));
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
    CHECK(!error.message.empty());
}

// --- P6: containment --------------------------------------------------------
//
// Removed with the 2026-10-07 trim: protoc writes wherever the mapped name
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
    test_unsafe_names_are_refused_by_the_mapping_not_the_publisher(root);
    test_publisher_reports_a_file_as_out_dir(root);
    test_publisher_rejects_an_existing_directory_at_the_target(root);
    test_publisher_keeps_old_content_when_a_later_replace_fails(root);

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all output tests passed\n";
    return EXIT_SUCCESS;
}
