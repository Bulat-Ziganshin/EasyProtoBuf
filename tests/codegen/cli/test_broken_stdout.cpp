// B9 / CR-14: a lost stdout write is a failure, not a success.
//
// The CLI lane cannot stage this one through `execute_process`: CMake always
// keeps the read end of its own output pipe open, so the child's writes always
// succeed. This test owns the pipe instead, hands the write end to the binary
// as stdout, closes the read end and asserts the transcript:
//
//   broken stdout   nonzero exit, empty-or-lost stdout, one diagnostic
//                   naming the stream ("cannot write to stdout"),
//   live stdout     the control: exit 0 and the whole header.
//
// On Windows a broken pipe answers ERROR_BROKEN_PIPE from WriteFile. On POSIX
// the write would raise SIGPIPE, whose default action kills the process before
// the stream can report anything, so the binary ignores the signal and the
// flush check turns the failed write into the diagnostic (main.cpp).
//
// The input is a descriptor set (`legacy.protodevel.pbs`), so the row runs in
// the parser-free profile too: the property is about the stream, not the
// frontend. Scratch lives below WORK_DIR and is overwritten on every run.

#include <cerrno>
#include <cstdio>
#include <cstring>
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
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

int failures = 0;

void check(bool condition, const char* expression, const char* file, int line)
{
    if (!condition) {
        std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", file, line,
                     expression);
        ++failures;
    }
}

#define CHECK(x) check((x), #x, __FILE__, __LINE__)

struct RunResult
{
    bool launched;
    unsigned long exit_code;
    std::string out;
    std::string err;

    RunResult() : launched(false), exit_code(0) {}
};

std::size_t newline_count(const std::string& text)
{
    std::size_t count = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') ++count;
    }
    return count;
}

#ifdef _WIN32

bool to_wide(const std::string& text, std::vector<wchar_t>& wide)
{
    wide.clear();
    if (text.empty()) return true;
    const int need = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                         text.data(),
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

std::string drain(HANDLE handle)
{
    std::string text;
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(handle, buffer, sizeof(buffer), &read, 0) && read > 0) {
        text.append(buffer, static_cast<std::size_t>(read));
    }
    return text;
}

// One spawn with the stream topology this row needs. `break_stdout` closes the
// read end before the child exists, so the pipe has no reader at all and every
// write fails immediately (size does not matter).
RunResult run_codegen(const std::string& codegen,
                      const std::vector<std::string>& arguments,
                      const std::string& work_dir, bool break_stdout)
{
    RunResult result;

    SECURITY_ATTRIBUTES attributes;
    std::memset(&attributes, 0, sizeof(attributes));
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE out_read = INVALID_HANDLE_VALUE;
    HANDLE out_write = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&out_read, &out_write, &attributes, 0)) return result;
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
    if (break_stdout) {
        CloseHandle(out_read);
        out_read = INVALID_HANDLE_VALUE;
    }

    HANDLE err_read = INVALID_HANDLE_VALUE;
    HANDLE err_write = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&err_read, &err_write, &attributes, 0)) {
        CloseHandle(out_write);
        if (out_read != INVALID_HANDLE_VALUE) CloseHandle(out_read);
        return result;
    }
    SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup;
    std::memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = out_write;
    startup.hStdError = err_write;

    std::string command_line = "\"" + codegen + "\"";
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        command_line += " \"" + arguments[i] + "\"";
    }
    std::vector<wchar_t> wide_command;
    std::vector<wchar_t> wide_work_dir;
    if (!to_wide(command_line, wide_command) ||
        !to_wide(work_dir, wide_work_dir)) {
        CloseHandle(out_write);
        CloseHandle(err_write);
        CloseHandle(err_read);
        if (out_read != INVALID_HANDLE_VALUE) CloseHandle(out_read);
        return result;
    }

    PROCESS_INFORMATION process;
    std::memset(&process, 0, sizeof(process));
    const BOOL started =
        CreateProcessW(0, &wide_command[0], 0, 0, TRUE, 0, 0,
                       wide_work_dir.empty() ? 0 : &wide_work_dir[0],
                       &startup, &process);
    CloseHandle(out_write);
    CloseHandle(err_write);
    if (!started) {
        CloseHandle(err_read);
        if (out_read != INVALID_HANDLE_VALUE) CloseHandle(out_read);
        return result;
    }

    if (out_read != INVALID_HANDLE_VALUE) {
        result.out = drain(out_read);
        CloseHandle(out_read);
    }
    result.err = drain(err_read);
    CloseHandle(err_read);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD status = 0;
    if (GetExitCodeProcess(process.hProcess, &status)) {
        result.exit_code = static_cast<unsigned long>(status);
        result.launched = true;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return result;
}

#else

std::string drain_fd(int descriptor)
{
    std::string text;
    char buffer[4096];
    for (;;) {
        const ssize_t got = ::read(descriptor, buffer, sizeof(buffer));
        if (got <= 0) break;
        text.append(buffer, static_cast<std::size_t>(got));
    }
    return text;
}

RunResult run_codegen(const std::string& codegen,
                      const std::vector<std::string>& arguments,
                      const std::string&, bool break_stdout)
{
    RunResult result;
    int out_pipe[2];
    if (::pipe(out_pipe) != 0) return result;
    // The read end is closed here, before the fork, so the child cannot
    // inherit it: without a reader in any process the pipe is broken for
    // every write.
    if (break_stdout) {
        ::close(out_pipe[0]);
        out_pipe[0] = -1;
    }
    int err_pipe[2];
    if (::pipe(err_pipe) != 0) {
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        return result;
    }

    const pid_t child = ::fork();
    if (child < 0) {
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        ::close(err_pipe[0]);
        ::close(err_pipe[1]);
        return result;
    }
    if (child == 0) {
        if (out_pipe[0] >= 0) ::close(out_pipe[0]);
        ::close(err_pipe[0]);
        ::dup2(out_pipe[1], STDOUT_FILENO);
        ::dup2(err_pipe[1], STDERR_FILENO);
        ::close(out_pipe[1]);
        ::close(err_pipe[1]);
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(codegen.c_str()));
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            argv.push_back(const_cast<char*>(arguments[i].c_str()));
        }
        argv.push_back(0);
        ::execv(codegen.c_str(), &argv[0]);
        ::_exit(127);
    }

    ::close(out_pipe[1]);
    ::close(err_pipe[1]);
    if (out_pipe[0] >= 0) {
        result.out = drain_fd(out_pipe[0]);
        ::close(out_pipe[0]);
    }
    result.err = drain_fd(err_pipe[0]);
    ::close(err_pipe[0]);

    int status = 0;
    if (::waitpid(child, &status, 0) != child) return result;
    result.launched = true;
    if (WIFEXITED(status)) {
        result.exit_code = static_cast<unsigned long>(WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        result.exit_code =
            static_cast<unsigned long>(128 + WTERMSIG(status));
    }
    return result;
}

#endif

bool make_directory(const std::string& path)
{
#ifdef _WIN32
    std::vector<wchar_t> wide;
    if (!to_wide(path, wide)) return false;
    return CreateDirectoryW(&wide[0], 0) != 0 ||
           GetLastError() == ERROR_ALREADY_EXISTS;
#else
    return ::mkdir(path.c_str(), 0777) == 0 || errno == EEXIST;
#endif
}

// The child runs with WORK_DIR as its working directory, so every other path
// has to be absolute before the spawn. CTest passes absolute paths already;
// this also keeps a by-hand run (`test.exe codegen.exe tests/... work`) honest.
std::string absolute_path(const std::string& path)
{
#ifdef _WIN32
    std::vector<wchar_t> wide;
    if (!to_wide(path, wide)) return path;
    const DWORD needed = GetFullPathNameW(&wide[0], 0, 0, 0);
    if (needed == 0) return path;
    std::vector<wchar_t> buffer(needed);
    const DWORD length = GetFullPathNameW(&wide[0], needed, &buffer[0], 0);
    if (length == 0 || length >= needed) return path;
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, &buffer[0],
                                          static_cast<int>(length), 0, 0, 0, 0);
    if (bytes <= 0) return path;
    std::string result(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, &buffer[0], static_cast<int>(length),
                        &result[0], bytes, 0, 0);
    return result;
#else
    if (!path.empty() && path[0] == '/') return path;
    char buffer[4096];
    if (::getcwd(buffer, sizeof(buffer)) == 0) return path;
    return std::string(buffer) + "/" + path;
#endif
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 4) {
        std::fprintf(stderr,
                     "usage: %s <codegen> <fixture-dir> <work-dir>\n",
                     argc > 0 ? argv[0] : "test");
        return 2;
    }
    const std::string codegen = absolute_path(argv[1]);
    const std::string fixture_dir = absolute_path(argv[2]);
    const std::string work_dir = absolute_path(argv[3]);
    if (!make_directory(work_dir)) {
        std::fprintf(stderr, "cannot create %s\n", work_dir.c_str());
        return 2;
    }

    const std::string input = fixture_dir + "/legacy.protodevel.pbs";
    std::vector<std::string> arguments;
    arguments.push_back("--descriptor-set");
    arguments.push_back(input);

    // The row: the caller's stdout is gone before the header is written.
    const RunResult broken =
        run_codegen(codegen, arguments, work_dir, true);
    CHECK(broken.launched);
    CHECK(broken.exit_code != 0);
    CHECK(broken.err.find("cannot write to stdout") != std::string::npos);
    CHECK(broken.err.find("Exception:") == std::string::npos);
    CHECK(newline_count(broken.err) == 1u);

    // The control: the same run with a live pipe delivers the whole header.
    const RunResult live = run_codegen(codegen, arguments, work_dir, false);
    CHECK(live.launched);
    CHECK(live.exit_code == 0);
    CHECK(live.err.empty());
    CHECK(live.out.find("Generated by EasyProtoBuf Codegen") !=
          std::string::npos);

    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("broken-stdout: %lu (broken) / %lu (live control)\n",
                broken.exit_code, live.exit_code);
    return 0;
}
