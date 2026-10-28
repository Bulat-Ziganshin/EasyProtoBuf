// S09 (V-34): the Windows writer must capture a write failure's Win32 code
// before it releases the handle. CloseHandle overwrites the thread's
// last-error value, so a writer that reads GetLastError() after the close can
// report 0 ("The operation completed successfully") instead of the write
// failure, and the diagnostic loses its cause.
//
// The natural sequence is not reachable through ordinary Win32 API use (the
// close of a valid handle succeeds and resets the last error), so the test
// compiles the unmodified codegen/file_paths.cpp with WriteFile wrapped to
// simulate a short successful write and CloseHandle wrapped to clear the last
// error after closing. The module is included rather than linked: linking
// easypb_header_output would define its symbols twice.

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

static BOOL WINAPI r13_write_file(HANDLE, LPCVOID, DWORD, LPDWORD,
                                  LPOVERLAPPED);
static BOOL WINAPI r13_close_handle(HANDLE);
#define WriteFile r13_write_file
#define CloseHandle r13_close_handle
#include "file_paths.cpp"
#undef WriteFile
#undef CloseHandle

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

enum WriteMode {
    WRITE_FULL,  // delegate to the real writer
    WRITE_SHORT  // accept half the request, then report ERROR_DISK_FULL
};

WriteMode mode = WRITE_FULL;

} // namespace

BOOL WINAPI r13_write_file(HANDLE handle, LPCVOID data, DWORD size,
                           LPDWORD written, LPOVERLAPPED overlapped)
{
    if (mode == WRITE_SHORT) {
        // A short write is the failure the loop must report. The code the
        // real call would leave in the thread's last error is set here; the
        // wrapped close below then clears it, exactly as a close does.
        const DWORD half = size / 2;
        if (!::WriteFile(handle, data, half, written, overlapped)) {
            return FALSE;
        }
        SetLastError(ERROR_DISK_FULL);
        return TRUE;
    }
    return ::WriteFile(handle, data, size, written, overlapped);
}

BOOL WINAPI r13_close_handle(HANDLE handle)
{
    const BOOL closed = ::CloseHandle(handle);
    // The hazard under test: the close replaces the last-error value, so a
    // caller that reads it afterwards sees 0 rather than the write's code.
    SetLastError(ERROR_SUCCESS);
    return closed;
}

int main()
{
    std::ostringstream name_text;
    name_text << "s09-windows-writer-" << GetCurrentProcessId();
    const std::string directory = name_text.str();
    std::string error;
    CHECK(easypb_file::make_directories(directory, error));

    const std::string target = directory + "/short.pb.hpp";

    // The control: an ordinary write still succeeds and reaches the disk.
    const std::string complete(4096u, 'x');
    CHECK(easypb_file::write_disk_file(target, complete, error));
    CHECK(error.empty());

    // A short write is a failure, and the diagnostic keeps the write's own
    // code instead of the close's cleared last error.
    mode = WRITE_SHORT;
    error.clear();
    const std::string contents(64u * 1024u, 'y');
    CHECK(!easypb_file::write_disk_file(target, contents, error));
    CHECK(error == "cannot write file (Win32 error 112)");

    ::DeleteFileA(target.c_str());
    ::RemoveDirectoryA(directory.c_str());

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::puts("all Windows writer tests passed");
    return EXIT_SUCCESS;
}
