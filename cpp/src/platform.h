#pragma once

/**
 * Cross-platform abstractions for Windows and POSIX systems.
 * Provides unified types and function wrappers.
 */

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
  #include <windows.h>
  #include <process.h>
  #include <io.h>
  #include <direct.h>
  #include <fcntl.h>
  // pid_t equivalent on Windows
  using pid_t = int;
  using uid_t = unsigned int;
  using gid_t = unsigned int;
    using ssize_t = SSIZE_T;
#else
  #include <fcntl.h>
  #include <pwd.h>
  #include <sys/types.h>
  #include <sys/wait.h>
  #include <unistd.h>
#endif

#include <ctime>
#include <optional>
#include <string>
#include <vector>

namespace nanoclaw {
namespace platform {

/**
 * Get the user's home directory.
 * Windows: %USERPROFILE% or %HOMEDRIVE%%HOMEPATH%
 * POSIX: $HOME or getpwuid()
 */
std::string get_home_dir();

/**
 * Get current user ID. Returns 1000 on Windows (non-root default).
 */
uid_t get_uid();

/**
 * Get current group ID. Returns 1000 on Windows.
 */
gid_t get_gid();

/**
 * Thread-safe gmtime. Converts time_t to UTC struct tm.
 */
bool gmtime_safe(const time_t* timer, struct tm* result);

/**
 * Thread-safe localtime. Converts time_t to local struct tm.
 */
bool localtime_safe(const time_t* timer, struct tm* result);

/**
 * Inverse of gmtime: convert UTC struct tm to time_t.
 * Portable replacement for POSIX timegm().
 */
time_t timegm_portable(struct tm* tm);

/**
 * Parse time string according to format (simplified strptime).
 * Supports: %Y, %m, %d, %H, %M, %S
 * Returns pointer past last consumed character, or nullptr on failure.
 */
const char* strptime_portable(const char* s, const char* format, struct tm* tm);

/**
 * Set an environment variable.
 */
void setenv_portable(const std::string& key, const std::string& value);

/**
 * Unset an environment variable.
 */
void unsetenv_portable(const std::string& key);

/**
 * Check if a timezone name is valid.
 */
bool is_valid_timezone_name(const std::string& tz);

/**
 * Check if a process is alive.
 */
bool is_process_alive(pid_t pid);

/**
 * Send termination signal to a process.
 */
void terminate_process(pid_t pid);

/**
 * Execute a command and capture its stdout output.
 * Cross-platform replacement for popen/pclose.
 */
std::string exec_command(const std::string& cmd);

/**
 * Execute a command and return its exit code.
 */
int system_command(const std::string& cmd);

/**
 * Redirect a command's stderr to null device.
 * Returns the modified command string.
 */
std::string suppress_stderr(const std::string& cmd);

// --- Process spawning abstraction ---

struct PipeHandles {
    int read_fd = -1;
    int write_fd = -1;
};

/**
 * Create a pipe. Returns true on success.
 */
bool create_pipe(PipeHandles& handles);

/**
 * Close a file descriptor.
 */
void close_fd(int fd);

/**
 * Write data to a file descriptor.
 */
ssize_t write_fd(int fd, const void* buf, size_t count);

/**
 * Read data from a file descriptor.
 */
ssize_t read_fd(int fd, void* buf, size_t count);

/**
 * Result of spawning a child process.
 */
struct SpawnResult {
    bool success = false;
    pid_t pid = -1;
    int stdin_fd = -1;   // parent writes to this
    int stdout_fd = -1;  // parent reads from this
    int stderr_fd = -1;  // parent reads from this
    std::string error;
#ifdef _WIN32
    HANDLE process_handle = INVALID_HANDLE_VALUE;
#endif
};

/**
 * Spawn a child process with redirected stdin/stdout/stderr.
 * The executable and args form the command line.
 */
SpawnResult spawn_process(const std::string& executable, const std::vector<std::string>& args);

/**
 * Spawn a detached (daemon) process with stdout/stderr redirected to files.
 * Stdin receives initial_input then is closed.
 * Returns pid on success, -1 on failure.
 */
struct DaemonResult {
    bool success = false;
    pid_t pid = -1;
    std::string error;
#ifdef _WIN32
    HANDLE process_handle = INVALID_HANDLE_VALUE;
#endif
};

DaemonResult spawn_daemon(
    const std::string& executable,
    const std::vector<std::string>& args,
    const std::string& working_dir,
    const std::string& stdout_path,
    const std::string& stderr_path,
    const std::string& initial_input = "");

/**
 * Wait for a child process to complete. Returns exit code.
 */
int wait_for_process(const SpawnResult& proc);

/**
 * Multiplexed read from two file descriptors (select/WaitForMultipleObjects).
 * Returns which fds are ready: bit 0 = fd1, bit 1 = fd2.
 * Returns 0 on timeout, -1 on error.
 */
int select_read(int fd1, bool fd1_open, int fd2, bool fd2_open, int timeout_ms);

/**
 * Open a file for writing (create/truncate). Returns fd or -1.
 */
int open_write_file(const std::string& path);

/**
 * Change working directory.
 */
bool change_dir(const std::string& path);

} // namespace platform
} // namespace nanoclaw
