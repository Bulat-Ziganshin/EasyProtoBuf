// U-27/U-37's POSIX regression: read_disk_file opens once, fstats the entry
// it actually opened, retries EINTR, keeps reading after a short count, and
// reports the errno text when a read fails.
//
// S09 (V-13/V-48) extends the same injection to the two ends of the call: the
// wrapper injects one EINTR into open() and can make close() report a delayed
// write error after a successful read.
//
// The test compiles the unmodified codegen/file_paths.cpp with read, open and
// close wrapped: the read wrapper injects one EINTR and caps the real os-level
// read at 1 KiB so the loop is exercised, and can switch to a hard EIO after
// the first chunk.
// A file truncated between fstat and read is not portable to stage; the 1 KiB
// cap proves the loop, and the EIO row proves that a failing read is no
// longer reported as a truncated success (the dead post-read guard this
// fixes).
//
// Windows never compiles this branch: the CMake target is registered only
// under NOT WIN32, and the body keeps the same guard so the split stays
// explicit. All standard headers file_paths.cpp includes are included here
// first, so the macro below cannot rewrite a standard library declaration.

#ifndef _WIN32

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static ssize_t r12_read(int, void*, std::size_t);
static int r12_open(const char*, int, ...);
static int r12_close(int);
#define read r12_read
#define open r12_open
#define close r12_close
#include "file_paths.cpp"
#undef read
#undef open
#undef close

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

enum ReadMode {
    READ_SUCCESS,        // EINTR once, then short successful reads
    READ_FAIL_AFTER_ONE  // EINTR once, one chunk, then EIO
};

ReadMode mode = READ_SUCCESS;
bool eintr_sent = false;
int data_reads = 0;
// S09 injection modes for the two ends of the call.
bool open_eintr_pending = false;
bool close_fails = false;

std::string test_path(const char* suffix)
{
    std::ostringstream name;
    name << "r12-posix-reader-" << ::getpid() << "-" << suffix;
    return name.str();
}

std::string make_contents(std::size_t size)
{
    std::string contents(size, 'x');
    for (std::size_t i = 0; i < contents.size(); ++i) {
        contents[i] = static_cast<char>('a' + (i % 26u));
    }
    return contents;
}

bool write_file(const std::string& path, const std::string& contents)
{
    std::ofstream output(path.c_str(), std::ios::out | std::ios::binary);
    if (!output) return false;
    output.write(contents.data(),
                 static_cast<std::streamsize>(contents.size()));
    output.close();
    return !output.fail();
}

} // namespace

ssize_t r12_read(int descriptor, void* data, std::size_t size)
{
    // Every row starts with one injected EINTR: the loop must retry it
    // instead of treating it as EOF or as a hard failure.
    if (!eintr_sent) {
        eintr_sent = true;
        errno = EINTR;
        return -1;
    }
    if (mode == READ_FAIL_AFTER_ONE && data_reads > 0) {
        errno = EIO;
        return -1;
    }
    ++data_reads;
    // Force short reads even when the kernel would take the whole chunk.
    const std::size_t cap = 1024u;
    return ::read(descriptor, data, std::min(size, cap));
}

int r12_open(const char* path, int flags, ...)
{
    if (open_eintr_pending) {
        open_eintr_pending = false;
        errno = EINTR;
        return -1;
    }
    mode_t creation_mode = 0;
    if ((flags & O_CREAT) != 0) {
        va_list arguments;
        va_start(arguments, flags);
        creation_mode = static_cast<mode_t>(va_arg(arguments, int));
        va_end(arguments);
    }
    return ::open(path, flags, creation_mode);
}

int r12_close(int descriptor)
{
    if (close_fails) {
        // Release the descriptor first, then report the delayed failure the
        // caller must not drop (V-48).
        ::close(descriptor);
        errno = EIO;
        return -1;
    }
    return ::close(descriptor);
}

int main()
{
    // A successful read crosses several short chunks; the injected EINTR is
    // retried, and the bytes are exact.
    const std::string success_path = test_path("success");
    const std::string contents = make_contents(5000);
    CHECK(write_file(success_path, contents));
    mode = READ_SUCCESS;
    eintr_sent = false;
    data_reads = 0;
    std::string error;
    std::string read_back;
    CHECK(easypb_file::read_disk_file(success_path, read_back, error));
    CHECK(error.empty());
    CHECK(eintr_sent);
    CHECK(data_reads > 1);
    CHECK(read_back == contents);

    // V-13: an interrupted open is retried; one EINTR must not read as an
    // unreadable file.
    open_eintr_pending = true;
    eintr_sent = false;
    data_reads = 0;
    error.clear();
    read_back.clear();
    CHECK(easypb_file::read_disk_file(success_path, read_back, error));
    CHECK(error.empty());
    CHECK(read_back == contents);
    CHECK(!open_eintr_pending);

    // V-48: a close that reports a delayed error turns an otherwise complete
    // read into a failure; the bytes are not published as a success.
    close_fails = true;
    eintr_sent = false;
    data_reads = 0;
    error.clear();
    read_back = "KEEP";
    CHECK(!easypb_file::read_disk_file(success_path, read_back, error));
    CHECK(error.find("cannot read file (errno ") != std::string::npos);
    CHECK(error.find(std::strerror(EIO)) != std::string::npos);
    CHECK(read_back.empty());
    close_fails = false;
    ::unlink(success_path.c_str());

    // A failing read is an error with the errno text, not truncated data
    // with success: this is the dead post-read guard U-27 removes.
    const std::string failing_path = test_path("failing");
    CHECK(write_file(failing_path, contents));
    mode = READ_FAIL_AFTER_ONE;
    eintr_sent = false;
    data_reads = 0;
    error.clear();
    read_back = "KEEP";
    CHECK(!easypb_file::read_disk_file(failing_path, read_back, error));
    CHECK(error.find("cannot read file (errno ") != std::string::npos);
    CHECK(error.find(std::strerror(EIO)) != std::string::npos);
    CHECK(read_back.empty());
    ::unlink(failing_path.c_str());

    // A missing file reports ENOENT's text, the OPTIONS.md promise that the
    // POSIX branch used to collapse into a bare "cannot read file".
    const std::string missing_path = test_path("missing");
    ::unlink(missing_path.c_str());
    error.clear();
    CHECK(!easypb_file::read_disk_file(missing_path, read_back, error));
    CHECK(error.find("cannot read file (errno ") != std::string::npos);
    CHECK(error.find(std::strerror(ENOENT)) != std::string::npos);

    // A directory is classified from the opened descriptor (fstat), so the
    // old stat-before-open window is gone and the message stays specific.
    const std::string directory_path = test_path("directory");
    ::rmdir(directory_path.c_str());
    CHECK(::mkdir(directory_path.c_str(), 0700) == 0);
    error.clear();
    CHECK(!easypb_file::read_disk_file(directory_path, read_back, error));
    CHECK(error == "is a directory, not a schema file");
    ::rmdir(directory_path.c_str());

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::puts("all POSIX reader tests passed");
    return EXIT_SUCCESS;
}

#else

int main()
{
    return 0;
}

#endif
