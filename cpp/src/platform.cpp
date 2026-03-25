#include "platform.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>

#ifdef _WIN32
  #include <shlobj.h>
  #include <tlhelp32.h>
  #pragma comment(lib, "shell32.lib")
#endif

namespace nanoclaw {
namespace platform {

// ============================================================
// Home directory
// ============================================================

std::string get_home_dir() {
#ifdef _WIN32
    // Try USERPROFILE first
    const char* userprofile = std::getenv("USERPROFILE");
    if (userprofile) return userprofile;

    // Fallback to HOMEDRIVE + HOMEPATH
    const char* homedrive = std::getenv("HOMEDRIVE");
    const char* homepath = std::getenv("HOMEPATH");
    if (homedrive && homepath) return std::string(homedrive) + homepath;

    // Last resort: SHGetFolderPath
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PROFILE, NULL, 0, path))) {
        return path;
    }
    return "C:\\Users\\Default";
#else
    const char* home = std::getenv("HOME");
    if (home) return home;
    struct passwd* pw = getpwuid(getuid());
    if (pw) return pw->pw_dir;
    return "/tmp";
#endif
}

// ============================================================
// User/Group IDs
// ============================================================

uid_t get_uid() {
#ifdef _WIN32
    return 1000; // Non-root default
#else
    return getuid();
#endif
}

gid_t get_gid() {
#ifdef _WIN32
    return 1000;
#else
    return getgid();
#endif
}

// ============================================================
// Time functions
// ============================================================

bool gmtime_safe(const time_t* timer, struct tm* result) {
#ifdef _WIN32
    return gmtime_s(result, timer) == 0;
#else
    return gmtime_r(timer, result) != nullptr;
#endif
}

bool localtime_safe(const time_t* timer, struct tm* result) {
#ifdef _WIN32
    return localtime_s(result, timer) == 0;
#else
    return localtime_r(timer, result) != nullptr;
#endif
}

time_t timegm_portable(struct tm* tm) {
#ifdef _WIN32
    return _mkgmtime(tm);
#else
    return timegm(tm);
#endif
}

const char* strptime_portable(const char* s, const char* format, struct tm* tm) {
#ifdef _WIN32
    // Simplified strptime implementation for common formats
    const char* sp = s;
    const char* fp = format;

    while (*fp && *sp) {
        if (*fp == '%') {
            fp++;
            switch (*fp) {
                case 'Y': {
                    char* end;
                    long val = strtol(sp, &end, 10);
                    if (end == sp) return nullptr;
                    tm->tm_year = static_cast<int>(val) - 1900;
                    sp = end;
                    break;
                }
                case 'm': {
                    char* end;
                    long val = strtol(sp, &end, 10);
                    if (end == sp) return nullptr;
                    tm->tm_mon = static_cast<int>(val) - 1;
                    sp = end;
                    break;
                }
                case 'd': {
                    char* end;
                    long val = strtol(sp, &end, 10);
                    if (end == sp) return nullptr;
                    tm->tm_mday = static_cast<int>(val);
                    sp = end;
                    break;
                }
                case 'H': {
                    char* end;
                    long val = strtol(sp, &end, 10);
                    if (end == sp) return nullptr;
                    tm->tm_hour = static_cast<int>(val);
                    sp = end;
                    break;
                }
                case 'M': {
                    char* end;
                    long val = strtol(sp, &end, 10);
                    if (end == sp) return nullptr;
                    tm->tm_min = static_cast<int>(val);
                    sp = end;
                    break;
                }
                case 'S': {
                    char* end;
                    long val = strtol(sp, &end, 10);
                    if (end == sp) return nullptr;
                    tm->tm_sec = static_cast<int>(val);
                    sp = end;
                    break;
                }
                default:
                    return nullptr;
            }
            fp++;
        } else {
            if (*sp != *fp) return nullptr;
            sp++;
            fp++;
        }
    }
    return sp;
#else
    return strptime(s, format, tm);
#endif
}

// ============================================================
// Environment variables
// ============================================================

void setenv_portable(const std::string& key, const std::string& value) {
#ifdef _WIN32
    _putenv_s(key.c_str(), value.c_str());
#else
    setenv(key.c_str(), value.c_str(), 1);
#endif
}

void unsetenv_portable(const std::string& key) {
#ifdef _WIN32
    _putenv_s(key.c_str(), "");
#else
    unsetenv(key.c_str());
#endif
}

// ============================================================
// Timezone validation
// ============================================================

bool is_valid_timezone_name(const std::string& tz) {
    if (tz.empty()) return false;
    if (tz == "UTC" || tz == "GMT") return true;

#ifdef _WIN32
    // On Windows, try setting the TZ env var and see if the C runtime accepts it.
    // Windows also supports IANA names via ICU in newer versions,
    // but for compatibility, we accept common formats.
    // Accept IANA-style names (e.g., "America/New_York")
    if (tz.find('/') != std::string::npos) {
        // Heuristic: IANA timezone names contain a slash
        // Windows 10 1903+ with ICU supports these via C++ <chrono>
        // For older systems, we just accept the format
        return true;
    }
    // Also accept POSIX-style abbreviations
    if (tz.size() >= 3 && tz.size() <= 5) return true;
    return false;
#else
    std::string tz_file = "/usr/share/zoneinfo/" + tz;
    FILE* f = fopen(tz_file.c_str(), "r");
    if (f) {
        fclose(f);
        return true;
    }
    return false;
#endif
}

// ============================================================
// Process management
// ============================================================

bool is_process_alive(pid_t pid) {
#ifdef _WIN32
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (hProcess == NULL) return false;
    DWORD exitCode;
    bool alive = GetExitCodeProcess(hProcess, &exitCode) && exitCode == STILL_ACTIVE;
    CloseHandle(hProcess);
    return alive;
#else
    return kill(pid, 0) == 0;
#endif
}

void terminate_process(pid_t pid) {
#ifdef _WIN32
    HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (hProcess != NULL) {
        TerminateProcess(hProcess, 1);
        CloseHandle(hProcess);
    }
#else
    kill(pid, SIGTERM);
#endif
}

// ============================================================
// Command execution
// ============================================================

std::string exec_command(const std::string& cmd) {
    std::array<char, 4096> buffer;
    std::string result;
#ifdef _WIN32
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) return "";
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        result += buffer.data();
    }
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return result;
}

int system_command(const std::string& cmd) {
    return std::system(cmd.c_str());
}

std::string suppress_stderr(const std::string& cmd) {
#ifdef _WIN32
    return cmd + " 2>NUL";
#else
    return cmd + " 2>/dev/null";
#endif
}

// ============================================================
// Pipe and I/O operations
// ============================================================

bool create_pipe(PipeHandles& handles) {
#ifdef _WIN32
    HANDLE read_handle, write_handle;
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;
    if (!CreatePipe(&read_handle, &write_handle, &sa, 0)) return false;
    handles.read_fd = _open_osfhandle(reinterpret_cast<intptr_t>(read_handle), _O_RDONLY);
    handles.write_fd = _open_osfhandle(reinterpret_cast<intptr_t>(write_handle), _O_WRONLY);
    return handles.read_fd != -1 && handles.write_fd != -1;
#else
    int fds[2];
    if (pipe(fds) < 0) return false;
    handles.read_fd = fds[0];
    handles.write_fd = fds[1];
    return true;
#endif
}

void close_fd(int fd) {
    if (fd >= 0) {
#ifdef _WIN32
        _close(fd);
#else
        close(fd);
#endif
    }
}

ssize_t write_fd(int fd, const void* buf, size_t count) {
#ifdef _WIN32
    return _write(fd, buf, static_cast<unsigned int>(count));
#else
    return write(fd, buf, count);
#endif
}

ssize_t read_fd(int fd, void* buf, size_t count) {
#ifdef _WIN32
    return _read(fd, buf, static_cast<unsigned int>(count));
#else
    return read(fd, buf, count);
#endif
}

int open_write_file(const std::string& path) {
#ifdef _WIN32
    return _open(path.c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC, _S_IREAD | _S_IWRITE);
#else
    return open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
#endif
}

bool change_dir(const std::string& path) {
#ifdef _WIN32
    return _chdir(path.c_str()) == 0;
#else
    return chdir(path.c_str()) == 0;
#endif
}

// ============================================================
// Process spawning
// ============================================================

#ifdef _WIN32

static std::string build_command_line(const std::string& executable, const std::vector<std::string>& args) {
    // Build a Windows command line string
    std::string cmd = "\"" + executable + "\"";
    for (const auto& arg : args) {
        cmd += " \"" + arg + "\"";
    }
    return cmd;
}

SpawnResult spawn_process(const std::string& executable, const std::vector<std::string>& args) {
    SpawnResult result;

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    // Create pipes for stdin, stdout, stderr
    HANDLE stdin_read, stdin_write;
    HANDLE stdout_read, stdout_write;
    HANDLE stderr_read, stderr_write;

    if (!CreatePipe(&stdin_read, &stdin_write, &sa, 0) ||
        !CreatePipe(&stdout_read, &stdout_write, &sa, 0) ||
        !CreatePipe(&stderr_read, &stderr_write, &sa, 0)) {
        result.error = "Failed to create pipes";
        return result;
    }

    // Ensure parent ends of pipes are not inherited
    SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.hStdInput = stdin_read;
    si.hStdOutput = stdout_write;
    si.hStdError = stderr_write;
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi = {};
    std::string cmd_line = build_command_line(executable, args);

    if (!CreateProcessA(
            NULL,
            const_cast<char*>(cmd_line.c_str()),
            NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(stdin_read); CloseHandle(stdin_write);
        CloseHandle(stdout_read); CloseHandle(stdout_write);
        CloseHandle(stderr_read); CloseHandle(stderr_write);
        result.error = "Failed to create process";
        return result;
    }

    // Close child-side handles in parent
    CloseHandle(stdin_read);
    CloseHandle(stdout_write);
    CloseHandle(stderr_write);
    CloseHandle(pi.hThread);

    result.success = true;
    result.pid = static_cast<pid_t>(pi.dwProcessId);
    result.stdin_fd = _open_osfhandle(reinterpret_cast<intptr_t>(stdin_write), _O_WRONLY);
    result.stdout_fd = _open_osfhandle(reinterpret_cast<intptr_t>(stdout_read), _O_RDONLY);
    result.stderr_fd = _open_osfhandle(reinterpret_cast<intptr_t>(stderr_read), _O_RDONLY);
    result.process_handle = pi.hProcess;
    return result;
}

DaemonResult spawn_daemon(
    const std::string& executable,
    const std::vector<std::string>& args,
    const std::string& working_dir,
    const std::string& stdout_path,
    const std::string& stderr_path,
    const std::string& initial_input)
{
    DaemonResult result;

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE stdout_handle = CreateFileA(stdout_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    HANDLE stderr_handle = CreateFileA(stderr_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (stdout_handle == INVALID_HANDLE_VALUE || stderr_handle == INVALID_HANDLE_VALUE) {
        if (stdout_handle != INVALID_HANDLE_VALUE) CloseHandle(stdout_handle);
        if (stderr_handle != INVALID_HANDLE_VALUE) CloseHandle(stderr_handle);
        result.error = "Failed to open output files";
        return result;
    }

    HANDLE stdin_read, stdin_write;
    if (!CreatePipe(&stdin_read, &stdin_write, &sa, 0)) {
        CloseHandle(stdout_handle);
        CloseHandle(stderr_handle);
        result.error = "Failed to create pipe";
        return result;
    }
    SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.hStdInput = stdin_read;
    si.hStdOutput = stdout_handle;
    si.hStdError = stderr_handle;
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi = {};
    std::string cmd_line = build_command_line(executable, args);

    if (!CreateProcessA(
            NULL,
            const_cast<char*>(cmd_line.c_str()),
            NULL, NULL, TRUE,
            CREATE_NEW_PROCESS_GROUP | DETACHED_PROCESS,
            NULL,
            working_dir.empty() ? NULL : working_dir.c_str(),
            &si, &pi)) {
        CloseHandle(stdin_read); CloseHandle(stdin_write);
        CloseHandle(stdout_handle); CloseHandle(stderr_handle);
        result.error = "Failed to create process";
        return result;
    }

    CloseHandle(stdin_read);
    CloseHandle(stdout_handle);
    CloseHandle(stderr_handle);
    CloseHandle(pi.hThread);

    // Write initial input
    if (!initial_input.empty()) {
        DWORD written;
        WriteFile(stdin_write, initial_input.c_str(),
            static_cast<DWORD>(initial_input.size()), &written, NULL);
    }
    CloseHandle(stdin_write);

    result.success = true;
    result.pid = static_cast<pid_t>(pi.dwProcessId);
    result.process_handle = pi.hProcess;
    return result;
}

int wait_for_process(const SpawnResult& proc) {
    if (proc.process_handle == INVALID_HANDLE_VALUE) return -1;
    WaitForSingleObject(proc.process_handle, INFINITE);
    DWORD exit_code;
    GetExitCodeProcess(proc.process_handle, &exit_code);
    CloseHandle(proc.process_handle);
    return static_cast<int>(exit_code);
}

int select_read(int fd1, bool fd1_open, int fd2, bool fd2_open, int timeout_ms) {
    // On Windows with CRT file descriptors, we use non-blocking reads with timeouts.
    // Since _read is blocking, we use a thread-based approach or polling.
    // Simplified: try reading with a short timeout via PeekNamedPipe.

    int ready = 0;
    if (fd1_open) {
        HANDLE h1 = reinterpret_cast<HANDLE>(_get_osfhandle(fd1));
        DWORD avail = 0;
        if (PeekNamedPipe(h1, NULL, 0, NULL, &avail, NULL) && avail > 0) {
            ready |= 1;
        }
    }
    if (fd2_open) {
        HANDLE h2 = reinterpret_cast<HANDLE>(_get_osfhandle(fd2));
        DWORD avail = 0;
        if (PeekNamedPipe(h2, NULL, 0, NULL, &avail, NULL) && avail > 0) {
            ready |= 2;
        }
    }

    if (ready == 0 && (fd1_open || fd2_open)) {
        // Brief sleep to avoid busy-waiting
        Sleep(static_cast<DWORD>(timeout_ms > 100 ? 100 : timeout_ms));

        // Check again
        if (fd1_open) {
            HANDLE h1 = reinterpret_cast<HANDLE>(_get_osfhandle(fd1));
            DWORD avail = 0;
            if (PeekNamedPipe(h1, NULL, 0, NULL, &avail, NULL) && avail > 0) {
                ready |= 1;
            }
        }
        if (fd2_open) {
            HANDLE h2 = reinterpret_cast<HANDLE>(_get_osfhandle(fd2));
            DWORD avail = 0;
            if (PeekNamedPipe(h2, NULL, 0, NULL, &avail, NULL) && avail > 0) {
                ready |= 2;
            }
        }
    }

    return ready;
}

#else // POSIX

SpawnResult spawn_process(const std::string& executable, const std::vector<std::string>& args) {
    SpawnResult result;

    int stdin_pipe[2], stdout_pipe[2], stderr_pipe[2];
    if (pipe(stdin_pipe) < 0 || pipe(stdout_pipe) < 0 || pipe(stderr_pipe) < 0) {
        result.error = "Failed to create pipes";
        return result;
    }

    pid_t pid = fork();
    if (pid < 0) {
        result.error = "Failed to fork";
        return result;
    }

    if (pid == 0) {
        // Child process
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);

        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);

        close(stdin_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);

        // Build argv
        std::vector<const char*> argv;
        argv.push_back(executable.c_str());
        for (const auto& arg : args) {
            argv.push_back(arg.c_str());
        }
        argv.push_back(nullptr);

        execvp(executable.c_str(), const_cast<char* const*>(argv.data()));
        _exit(127);
    }

    // Parent process
    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    result.success = true;
    result.pid = pid;
    result.stdin_fd = stdin_pipe[1];
    result.stdout_fd = stdout_pipe[0];
    result.stderr_fd = stderr_pipe[0];
    return result;
}

DaemonResult spawn_daemon(
    const std::string& executable,
    const std::vector<std::string>& args,
    const std::string& working_dir,
    const std::string& stdout_path,
    const std::string& stderr_path,
    const std::string& initial_input)
{
    DaemonResult result;

    int stdout_fd = open(stdout_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int stderr_fd = open(stderr_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (stdout_fd < 0 || stderr_fd < 0) {
        if (stdout_fd >= 0) close(stdout_fd);
        if (stderr_fd >= 0) close(stderr_fd);
        result.error = "Failed to open output files";
        return result;
    }

    int stdin_pipe[2];
    if (pipe(stdin_pipe) < 0) {
        close(stdout_fd);
        close(stderr_fd);
        result.error = "Failed to create pipe";
        return result;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdout_fd);
        close(stderr_fd);
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        result.error = "Failed to fork";
        return result;
    }

    if (pid == 0) {
        // Child
        setsid();
        close(stdin_pipe[1]);
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_fd, STDOUT_FILENO);
        dup2(stderr_fd, STDERR_FILENO);
        close(stdin_pipe[0]);
        close(stdout_fd);
        close(stderr_fd);

        if (!working_dir.empty() && chdir(working_dir.c_str()) != 0) _exit(1);

        std::vector<const char*> argv;
        argv.push_back(executable.c_str());
        for (const auto& a : args) argv.push_back(a.c_str());
        argv.push_back(nullptr);

        execvp(executable.c_str(), const_cast<char* const*>(argv.data()));
        _exit(127);
    }

    // Parent
    close(stdin_pipe[0]);
    if (!initial_input.empty()) {
        write(stdin_pipe[1], initial_input.c_str(), initial_input.size());
    }
    close(stdin_pipe[1]);
    close(stdout_fd);
    close(stderr_fd);

    result.success = true;
    result.pid = pid;
    return result;
}

int wait_for_process(const SpawnResult& proc) {
    int status = 0;
    waitpid(proc.pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int select_read(int fd1, bool fd1_open, int fd2, bool fd2_open, int timeout_ms) {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    int max_fd = -1;

    if (fd1_open) {
        FD_SET(fd1, &read_fds);
        max_fd = std::max(max_fd, fd1);
    }
    if (fd2_open) {
        FD_SET(fd2, &read_fds);
        max_fd = std::max(max_fd, fd2);
    }
    if (max_fd < 0) return 0;

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int ret = select(max_fd + 1, &read_fds, nullptr, nullptr, &tv);
    if (ret <= 0) return ret;

    int ready = 0;
    if (fd1_open && FD_ISSET(fd1, &read_fds)) ready |= 1;
    if (fd2_open && FD_ISSET(fd2, &read_fds)) ready |= 2;
    return ready;
}

#endif // _WIN32

} // namespace platform
} // namespace nanoclaw
