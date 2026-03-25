#include "remote_control.h"
#include "config.h"
#include "logger.h"
#include "platform.h"
#include "timezone.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <thread>

namespace nanoclaw {

namespace fs = std::filesystem;
using json = nlohmann::json;

static std::optional<RemoteControlSession> g_active_session;
#ifdef _WIN32
static HANDLE g_active_process_handle = INVALID_HANDLE_VALUE;
#endif

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
#ifdef _WIN32
    if (g_active_process_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(g_active_process_handle);
        g_active_process_handle = INVALID_HANDLE_VALUE;
    }
#endif
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

        if (session.pid > 0 && platform::is_process_alive(session.pid)) {
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
        if (platform::is_process_alive(g_active_session->pid)) {
            return {true, g_active_session->url, ""};
        }
        g_active_session.reset();
        clear_state();
    }

    fs::create_directories(config().data_dir);

    auto daemon = platform::spawn_daemon(
        "claude",
        {"remote-control", "--name", "NanoClaw Remote"},
        cwd,
        stdout_file(),
        stderr_file(),
        "y\n");

    if (!daemon.success) {
        return {false, "", daemon.error};
    }

    pid_t pid = daemon.pid;
#ifdef _WIN32
    g_active_process_handle = daemon.process_handle;
#endif

    // Poll for URL
    auto start = std::chrono::steady_clock::now();
    while (true) {
        if (!platform::is_process_alive(pid)) {
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
            platform::terminate_process(pid);
            return {false, "", "Timed out waiting for Remote Control URL"};
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(URL_POLL_MS));
    }
}

RemoteControlResult stop_remote_control() {
    if (!g_active_session) {
        return {false, "", "No active Remote Control session"};
    }

    platform::terminate_process(g_active_session->pid);
    logger()->info("Remote Control session stopped: pid={}", g_active_session->pid);
    g_active_session.reset();
    clear_state();
    return {true, "", ""};
}

} // namespace nanoclaw
