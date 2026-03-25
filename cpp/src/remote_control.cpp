#include "remote_control.h"
#include "config.h"
#include "logger.h"
#include "timezone.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace nanoclaw {

namespace fs = std::filesystem;
using json = nlohmann::json;

static std::optional<RemoteControlSession> g_active_session;

static const std::regex URL_REGEX(R"(https://claude\.ai/code\S+)");
static const int URL_TIMEOUT_MS = 30000;
static const int URL_POLL_MS = 200;

static std::string state_file() {
    return (fs::path(config().data_dir) / "remote-control.json").string();
}
static std::string stdout_file() {
    return (fs::path(config().data_dir) / "remote-control.stdout").string();
}
static std::string stderr_file() {
    return (fs::path(config().data_dir) / "remote-control.stderr").string();
}

static bool is_process_alive(int pid) {
    return kill(pid, 0) == 0;
}

static void save_state(const RemoteControlSession& session) {
    fs::create_directories(fs::path(state_file()).parent_path());
    json j;
    j["pid"] = session.pid;
    j["url"] = session.url;
    j["startedBy"] = session.started_by;
    j["startedInChat"] = session.started_in_chat;
    j["startedAt"] = session.started_at;
    std::ofstream(state_file()) << j.dump();
}

static void clear_state() {
    try { fs::remove(state_file()); } catch (...) {}
}

void restore_remote_control() {
    std::ifstream file(state_file());
    if (!file.is_open()) return;

    try {
        auto j = json::parse(file);
        RemoteControlSession session;
        session.pid = j["pid"].get<int>();
        session.url = j["url"].get<std::string>();
        session.started_by = j.value("startedBy", "");
        session.started_in_chat = j.value("startedInChat", "");
        session.started_at = j.value("startedAt", "");

        if (session.pid > 0 && is_process_alive(session.pid)) {
            g_active_session = session;
            logger()->info("Restored Remote Control session: pid={}, url={}", session.pid, session.url);
        } else {
            clear_state();
        }
    } catch (...) {
        clear_state();
    }
}

std::optional<RemoteControlSession> get_active_session() {
    return g_active_session;
}

RemoteControlResult start_remote_control(
    const std::string& sender,
    const std::string& chat_jid,
    const std::string& cwd)
{
    if (g_active_session) {
        if (is_process_alive(g_active_session->pid)) {
            return {true, g_active_session->url, ""};
        }
        g_active_session.reset();
        clear_state();
    }

    fs::create_directories(config().data_dir);

    int stdout_fd = open(stdout_file().c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int stderr_fd = open(stderr_file().c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (stdout_fd < 0 || stderr_fd < 0) {
        if (stdout_fd >= 0) close(stdout_fd);
        if (stderr_fd >= 0) close(stderr_fd);
        return {false, "", "Failed to open output files"};
    }

    int stdin_pipe[2];
    if (pipe(stdin_pipe) < 0) {
        close(stdout_fd);
        close(stderr_fd);
        return {false, "", "Failed to create pipe"};
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdout_fd);
        close(stderr_fd);
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        return {false, "", "Failed to fork"};
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

        if (chdir(cwd.c_str()) != 0) _exit(1);
        execlp("claude", "claude", "remote-control", "--name", "NanoClaw Remote", nullptr);
        _exit(127);
    }

    // Parent
    close(stdin_pipe[0]);
    write(stdin_pipe[1], "y\n", 2);
    close(stdin_pipe[1]);
    close(stdout_fd);
    close(stderr_fd);

    // Poll for URL
    auto start = std::chrono::steady_clock::now();
    while (true) {
        if (!is_process_alive(pid)) {
            return {false, "", "Process exited before producing URL"};
        }

        std::ifstream f(stdout_file());
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

        std::smatch match;
        if (std::regex_search(content, match, URL_REGEX)) {
            RemoteControlSession session;
            session.pid = pid;
            session.url = match[0].str();
            session.started_by = sender;
            session.started_in_chat = chat_jid;
            session.started_at = now_iso();
            g_active_session = session;
            save_state(session);
            logger()->info("Remote Control session started: pid={}, url={}", pid, session.url);
            return {true, session.url, ""};
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= URL_TIMEOUT_MS) {
            kill(pid, SIGTERM);
            return {false, "", "Timed out waiting for Remote Control URL"};
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(URL_POLL_MS));
    }
}

RemoteControlResult stop_remote_control() {
    if (!g_active_session) {
        return {false, "", "No active Remote Control session"};
    }

    kill(g_active_session->pid, SIGTERM);
    logger()->info("Remote Control session stopped: pid={}", g_active_session->pid);
    g_active_session.reset();
    clear_state();
    return {true, "", ""};
}

} // namespace nanoclaw
