// W-31's Windows regression: a working-directory query that fails must be a
// refusal, not a silent anchor at the filesystem root.
//
// The natural failure is not reachable through ordinary Win32 API use (G3
// tried a subst drive and a deleted directory), so the test compiles the
// unmodified codegen/file_paths.cpp with GetCurrentDirectoryW stubbed to the
// failure the code already handles. The module is included rather than
// linked: linking easypb_header_output would define its symbols twice.

#include <windows.h>

extern "C" DWORD WINAPI FakeGetCurrentDirectoryW(DWORD, LPWSTR)
{
    SetLastError(ERROR_PATH_NOT_FOUND);
    return 0;
}

#define GetCurrentDirectoryW FakeGetCurrentDirectoryW
#include "file_paths.cpp"
#undef GetCurrentDirectoryW

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

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

} // namespace

int main()
{
    std::ostringstream name_text;
    name_text << "s7b-cwd-probe-" << GetCurrentProcessId();
    const std::string name = name_text.str();

    // A relative directory spelling cannot be resolved, and the diagnostic
    // says why instead of creating a root-anchored directory.
    std::string error;
    CHECK(!easypb_file::make_directories(name, error));
    CHECK(error.find("current working directory") != std::string::npos);
    CHECK(easypb_file::lexical_absolute(name).empty());

    // The writer is the same rule: it refuses rather than reaching C:/.
    error.clear();
    CHECK(!easypb_file::write_disk_file(name + ".pb.hpp", "X", error));
    CHECK(error.find("cannot write file") != std::string::npos);

    // No root-anchored entry was created. A failed run of an older build is
    // cleaned up after the check so a rerun starts clean.
    const std::string rooted = "C:/" + name;
    const std::string rooted_file = rooted + ".pb.hpp";
    const DWORD attributes = GetFileAttributesA(rooted.c_str());
    const DWORD file_attributes = GetFileAttributesA(rooted_file.c_str());
    CHECK(attributes == INVALID_FILE_ATTRIBUTES);
    CHECK(file_attributes == INVALID_FILE_ATTRIBUTES);
    if (attributes != INVALID_FILE_ATTRIBUTES) RemoveDirectoryA(rooted.c_str());
    if (file_attributes != INVALID_FILE_ATTRIBUTES) {
        DeleteFileA(rooted_file.c_str());
    }

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::puts("all cwd-failure tests passed");
    return EXIT_SUCCESS;
}
