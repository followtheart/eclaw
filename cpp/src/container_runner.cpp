#include "container_runner.h"
#include "config.h"
#include "group_folder.h"
#include "logger.h"
#include "mount_security.h"
#include "platform.h"
#include "timezone.h"

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace nanoclaw {

namespace fs = std::filesystem;
using json = nlohmann::json;

static const std::string OUTPUT_START_MARKER = "---NANOCLAW_OUTPUT_START---";
static const std::string OUTPUT_END_MARKER = "---NANOCLAW_OUTPUT_END---";

/**
 * Prepare directories and environment for direct agent-runner execution.
 * Returns the working directory (group dir) and sets up Claude sessions/skills.
 */
static std::string prepare_agent_environment(const RegisteredGroup& group, bool is_main) {
    auto project_root = fs::current_path().string();
    auto group_dir = resolve_group_folder_path(group.folder);
    const auto& cfg = config();

    // Per-group Claude sessions directory
    auto group_sessions_dir = (fs::path(cfg.data_dir) / "sessions" / group.folder / ".claude").string();
    fs::create_directories(group_sessions_dir);

    auto settings_file = fs::path(group_sessions_dir) / "settings.json";
    if (!fs::exists(settings_file)) {
        json settings;
        settings["env"] = {
            {"CLAUDE_CODE_EXPERIMENTAL_AGENT_TEAMS", "1"},
            {"CLAUDE_CODE_ADDITIONAL_DIRECTORIES_CLAUDE_MD", "1"},
            {"CLAUDE_CODE_DISABLE_AUTO_MEMORY", "0"}
        };
        std::ofstream(settings_file.string()) << settings.dump(2) << "\n";
    }

    // Sync skills
    auto skills_src = fs::path(project_root) / "container" / "skills";
    auto skills_dst = fs::path(group_sessions_dir) / "skills";
    if (fs::exists(skills_src)) {
        for (const auto& entry : fs::directory_iterator(skills_src)) {
            if (entry.is_directory()) {
                auto dst = skills_dst / entry.path().filename();
                fs::copy(entry.path(), dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            }
        }
    }

    // Per-group IPC namespace
    auto group_ipc_dir = resolve_group_ipc_path(group.folder);
    fs::create_directories(fs::path(group_ipc_dir) / "messages");
    fs::create_directories(fs::path(group_ipc_dir) / "tasks");
    fs::create_directories(fs::path(group_ipc_dir) / "input");

    // Agent runner source — copy once
    auto agent_runner_src = (fs::path(project_root) / "container" / "agent-runner" / "src").string();
    auto group_agent_runner = (fs::path(cfg.data_dir) / "sessions" / group.folder / "agent-runner-src").string();
    if (!fs::exists(group_agent_runner) && fs::exists(agent_runner_src)) {
        fs::copy(agent_runner_src, group_agent_runner, fs::copy_options::recursive);
    }

    return group_dir;
}

ContainerOutput run_container_agent(
    const RegisteredGroup& group,
    const ContainerInput& input,
    OnProcessCallback on_process,
    OnOutputCallback on_output)
{
    auto start_time = std::chrono::steady_clock::now();
    const auto& cfg = config();

    auto group_dir = resolve_group_folder_path(group.folder);
    fs::create_directories(group_dir);

    prepare_agent_environment(group, input.is_main);

    std::string safe_name = group.folder;
    for (auto& c : safe_name) {
        if (!std::isalnum(c) && c != '-') c = '-';
    }
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string process_name = "nanoclaw-" + safe_name + "-" + std::to_string(now_ms);

    logger()->info("Spawning agent process: group={}, name={}",
        group.name, process_name);

    auto logs_dir = fs::path(group_dir) / "logs";
    fs::create_directories(logs_dir);

    // Prepare input JSON
    json input_json;
    input_json["prompt"] = input.prompt;
    if (input.session_id) input_json["sessionId"] = *input.session_id;
    input_json["groupFolder"] = input.group_folder;
    input_json["chatJid"] = input.chat_jid;
    input_json["isMain"] = input.is_main;
    if (input.is_scheduled_task) input_json["isScheduledTask"] = true;
    if (!input.assistant_name.empty()) input_json["assistantName"] = input.assistant_name;
    std::string input_str = input_json.dump();

    // Build args: node <agent_runner_path>
    std::vector<std::string> args = {cfg.agent_runner_path};

    // Set environment variables for the agent-runner to know paths
    auto project_root = fs::current_path().string();
    auto group_ipc_dir = resolve_group_ipc_path(group.folder);
    auto group_sessions_dir = (fs::path(cfg.data_dir) / "sessions" / group.folder / ".claude").string();

    // Set env vars so the agent-runner can find its workspace
    // These replace the Docker volume mount mapping
    platform::setenv_portable("NANOCLAW_GROUP_DIR", group_dir);
    platform::setenv_portable("NANOCLAW_IPC_DIR", group_ipc_dir);
    platform::setenv_portable("NANOCLAW_PROJECT_DIR", project_root);
    platform::setenv_portable("NANOCLAW_CLAUDE_DIR", group_sessions_dir);
    platform::setenv_portable("TZ", cfg.timezone);

    // Additional directories from group config
    if (group.container_config) {
        auto validated = validate_additional_mounts(group.container_config->additional_mounts, group.name, input.is_main);
        if (!validated.empty()) {
            std::string extra_dirs;
            for (size_t i = 0; i < validated.size(); ++i) {
                if (i > 0) extra_dirs += ";";
                extra_dirs += validated[i].host_path;
            }
            platform::setenv_portable("NANOCLAW_EXTRA_DIRS", extra_dirs);
        }
    }

    // Spawn node process directly
    auto proc = platform::spawn_process("node", args);
    if (!proc.success) {
        return {"error", std::nullopt, std::nullopt, proc.error};
    }

    on_process(proc.pid, process_name);

    // Write input to stdin and close
    platform::write_fd(proc.stdin_fd, input_str.c_str(), input_str.size());
    platform::close_fd(proc.stdin_fd);

    // Read stdout and stderr
    std::string stdout_buf, stderr_buf;
    std::string parse_buffer;
    std::optional<std::string> new_session_id;
    bool had_streaming_output = false;

    bool stdout_open = true, stderr_open = true;

    while (stdout_open || stderr_open) {
        int ready = platform::select_read(
            proc.stdout_fd, stdout_open,
            proc.stderr_fd, stderr_open,
            1000);
        if (ready < 0) break;

        if (stdout_open && (ready & 1)) {
            char buf[4096];
            auto n = platform::read_fd(proc.stdout_fd, buf, sizeof(buf));
            if (n <= 0) {
                stdout_open = false;
            } else {
                std::string chunk(buf, n);
                stdout_buf += chunk;

                if (on_output) {
                    parse_buffer += chunk;
                    size_t start_idx;
                    while ((start_idx = parse_buffer.find(OUTPUT_START_MARKER)) != std::string::npos) {
                        auto end_idx = parse_buffer.find(OUTPUT_END_MARKER, start_idx);
                        if (end_idx == std::string::npos) break;

                        auto json_str = parse_buffer.substr(
                            start_idx + OUTPUT_START_MARKER.size(),
                            end_idx - start_idx - OUTPUT_START_MARKER.size());

                        // Trim
                        size_t first = json_str.find_first_not_of(" \t\n\r");
                        size_t last = json_str.find_last_not_of(" \t\n\r");
                        if (first != std::string::npos) json_str = json_str.substr(first, last - first + 1);

                        parse_buffer = parse_buffer.substr(end_idx + OUTPUT_END_MARKER.size());

                        try {
                            auto parsed = json::parse(json_str);
                            ContainerOutput output;
                            output.status = parsed.value("status", "success");
                            if (parsed.contains("result") && !parsed["result"].is_null()) {
                                output.result = parsed["result"].get<std::string>();
                            }
                            if (parsed.contains("newSessionId") && !parsed["newSessionId"].is_null()) {
                                output.new_session_id = parsed["newSessionId"].get<std::string>();
                                new_session_id = output.new_session_id;
                            }
                            if (parsed.contains("error") && !parsed["error"].is_null()) {
                                output.error = parsed["error"].get<std::string>();
                            }
                            had_streaming_output = true;
                            on_output(output);
                        } catch (const std::exception& e) {
                            logger()->warn("Failed to parse streamed output: {}", e.what());
                        }
                    }
                }
            }
        }

        if (stderr_open && (ready & 2)) {
            char buf[4096];
            auto n = platform::read_fd(proc.stderr_fd, buf, sizeof(buf));
            if (n <= 0) {
                stderr_open = false;
            } else {
                stderr_buf.append(buf, n);
            }
        }
    }

    platform::close_fd(proc.stdout_fd);
    platform::close_fd(proc.stderr_fd);

    // Wait for child
    int exit_code = platform::wait_for_process(proc);

    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    // Write log
    auto ts = now_iso();
    for (auto& c : ts) { if (c == ':' || c == '.') c = '-'; }
    auto log_file = (logs_dir / ("agent-" + ts + ".log")).string();
    {
        std::ofstream log(log_file);
        log << "=== Agent Run Log ===\n"
            << "Timestamp: " << now_iso() << "\n"
            << "Group: " << group.name << "\n"
            << "IsMain: " << (input.is_main ? "true" : "false") << "\n"
            << "Duration: " << duration << "ms\n"
            << "Exit Code: " << exit_code << "\n";
        if (exit_code != 0) {
            log << "\n=== Stderr ===\n" << stderr_buf << "\n";
        }
    }

    if (exit_code != 0) {
        logger()->error("Agent exited with error: group={}, code={}, duration={}ms", group.name, exit_code, duration);
        return {"error", std::nullopt, std::nullopt,
            "Agent exited with code " + std::to_string(exit_code) + ": " + stderr_buf.substr(0, std::min(stderr_buf.size(), size_t(200)))};
    }

    // Streaming mode
    if (on_output) {
        logger()->info("Agent completed (streaming mode): group={}, duration={}ms", group.name, duration);
        return {"success", std::nullopt, new_session_id};
    }

    // Legacy mode: parse last output marker
    auto start_idx = stdout_buf.find(OUTPUT_START_MARKER);
    auto end_idx = stdout_buf.find(OUTPUT_END_MARKER);
    if (start_idx != std::string::npos && end_idx != std::string::npos && end_idx > start_idx) {
        auto json_str = stdout_buf.substr(start_idx + OUTPUT_START_MARKER.size(),
            end_idx - start_idx - OUTPUT_START_MARKER.size());
        try {
            auto parsed = json::parse(json_str);
            ContainerOutput output;
            output.status = parsed.value("status", "success");
            if (parsed.contains("result") && !parsed["result"].is_null()) output.result = parsed["result"].get<std::string>();
            if (parsed.contains("newSessionId") && !parsed["newSessionId"].is_null()) output.new_session_id = parsed["newSessionId"].get<std::string>();
            if (parsed.contains("error") && !parsed["error"].is_null()) output.error = parsed["error"].get<std::string>();
            return output;
        } catch (...) {}
    }

    return {"error", std::nullopt, std::nullopt, "Failed to parse agent output"};
}

void write_tasks_snapshot(
    const std::string& group_folder,
    bool is_main,
    const std::vector<TaskSnapshot>& tasks)
{
    auto group_ipc_dir = resolve_group_ipc_path(group_folder);
    fs::create_directories(group_ipc_dir);

    json filtered = json::array();
    for (const auto& t : tasks) {
        if (is_main || t.group_folder == group_folder) {
            json task;
            task["id"] = t.id;
            task["groupFolder"] = t.group_folder;
            task["prompt"] = t.prompt;
            task["schedule_type"] = t.schedule_type;
            task["schedule_value"] = t.schedule_value;
            task["status"] = t.status;
            if (t.next_run) task["next_run"] = *t.next_run;
            else task["next_run"] = nullptr;
            filtered.push_back(task);
        }
    }

    auto tasks_file = fs::path(group_ipc_dir) / "current_tasks.json";
    std::ofstream(tasks_file.string()) << filtered.dump(2);
}

void write_groups_snapshot(
    const std::string& group_folder,
    bool is_main,
    const std::vector<AvailableGroup>& groups,
    const std::set<std::string>& /*registered_jids*/)
{
    auto group_ipc_dir = resolve_group_ipc_path(group_folder);
    fs::create_directories(group_ipc_dir);

    json j;
    j["groups"] = json::array();
    if (is_main) {
        for (const auto& g : groups) {
            json grp;
            grp["jid"] = g.jid;
            grp["name"] = g.name;
            grp["lastActivity"] = g.last_activity;
            grp["isRegistered"] = g.is_registered;
            j["groups"].push_back(grp);
        }
    }
    j["lastSync"] = now_iso();

    auto groups_file = fs::path(group_ipc_dir) / "available_groups.json";
    std::ofstream(groups_file.string()) << j.dump(2);
}

} // namespace nanoclaw
