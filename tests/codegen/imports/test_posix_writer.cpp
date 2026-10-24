// W-33's POSIX regression: the writer retries EINTR, refuses a zero-length
// write as an error instead of looping forever, and never asks for more than
// 64 KiB in one call.
//
// The test compiles the unmodified codegen/file_paths.cpp with write wrapped:
// the wrapper injects one EINTR, caps the real os-level write at 16 KiB so
// the partial-write path is exercised, and can switch to a zero return. All
// standard headers file_paths.cpp includes are included here first, so the
// macro below cannot rewrite a standard library declaration.

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

static ssize_t s7b_write(int, const void*, std::size_t);
#define write s7b_write
#include "file_paths.cpp"
#undef write

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

int mode = 0;             // 0: success (one EINTR), 1: zero return
bool eintr_sent = false;
std::size_t largest_request = 0;

bool read_file(const std::string& path, std::string& contents)
{
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input) return false;
    contents.assign(std::istreambuf_iterator<char>(input),
                    std::istreambuf_iterator<char>());
    return input.good() || input.eof();
}

std::string test_path(const char* suffix)
{
    std::ostringstream name;
    name << "s7b-posix-writer-" << ::getpid() << "-" << suffix;
    return name.str();
}

} // namespace

ssize_t s7b_write(int descriptor, const void* data, std::size_t size)
{
    if (size > largest_request) largest_request = size;
    if (mode == 1) return 0;
    if (!eintr_sent) {
        eintr_sent = true;
        errno = EINTR;
        return -1;
    }
    // Force partial writes even when the kernel would take the whole chunk.
    const std::size_t cap = 16u * 1024u;
    const std::size_t chunk = std::min(size, cap);
    return ::write(descriptor, data, chunk);
}

int main()
{
    const std::string success_target = test_path("success");
    std::string contents(200u * 1024u, 'x');
    for (std::size_t i = 0; i < contents.size(); ++i) {
        contents[i] = static_cast<char>('a' + (i % 26u));
    }

    mode = 0;
    eintr_sent = false;
    largest_request = 0;
    std::string error;
    CHECK(easypb_file::write_disk_file(success_target, contents, error));
    CHECK(error.empty());
    CHECK(eintr_sent);                        // the injected EINTR was retried
    CHECK(largest_request <= 64u * 1024u);    // the chunk cap holds
    CHECK(largest_request > 0);

    std::string written;
    CHECK(read_file(success_target, written));
    CHECK(written == contents);
    ::unlink(success_target.c_str());

    const std::string zero_target = test_path("zero");
    mode = 1;
    eintr_sent = false;
    largest_request = 0;
    error.clear();
    CHECK(!easypb_file::write_disk_file(zero_target, "CONTENT", error));
    CHECK(error.find("the write made no progress") != std::string::npos);
    ::unlink(zero_target.c_str());

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::puts("all POSIX writer tests passed");
    return EXIT_SUCCESS;
}
