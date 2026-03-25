#include "container_runner.h"
#include "config.h"
#include "container_runtime.h"
#include "group_folder.h"
#include "logger.h"
#include "mount_security.h"
#include "timezone.h"

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

namespace nanoclaw {

namespace fs = std::filesystem;
using json = nlohmann::json;

static const std::string OUTPUT_START_MARKER = "---NANOCLAW_OUTPUT_START---";
static const std::string OUTPUT_END_MARKER = "---NANOCLAW_OUTPUT_END---";

static std::vector<VolumeMount> build_volume_mounts(const RegisteredGroup& group, bool is_main) {
    std::vector<VolumeMount> mounts;
    auto project_root = fs::current_path().string();
    auto group_dir = resolve_group_folder_path(group.folder);
    const auto& cfg = config();

    if (is_main) {
        mounts.push_back({project_root, "/workspace/project", true});

        // Shadow .env
        auto env_file = fs::path(project_root) / ".env";
        if (fs::exists(env_file)) {
            mounts.push_back({"/dev/null", "/workspace/project/.env", true});
        }

        mounts.push_back({group_dir, "/workspace/group", false});
    } else {
        mounts.push_back({group_dir, "/workspace/group", false});

        auto global_dir = (fs::path(cfg.groups_dir) / "global").string();
        if (fs::exists(global_dir)) {
            mounts.push_back({global_dir, "/workspace/global", true});
        }
    }

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

    mounts.push_back({group_sessions_dir, "/home/node/.claude", false});

    // Per-group IPC namespace
    auto group_ipc_dir = resolve_group_ipc_path(group.folder);
    fs::create_directories(fs::path(group_ipc_dir) / "messages");
    fs::create_directories(fs::path(group_ipc_dir) / "tasks");
    fs::create_directories(fs::path(group_ipc_dir) / "input");
    mounts.push_back({group_ipc_dir, "/workspace/ipc", false});

    // Agent runner source
    auto agent_runner_src = (fs::path(project_root) / "container" / "agent-runner" / "src").string();
    auto group_agent_runner = (fs::path(cfg.data_dir) / "sessions" / group.folder / "agent-runner-src").string();
    if (!fs::exists(group_agent_runner) && fs::exists(agent_runner_src)) {
        fs::copy(agent_runner_src, group_agent_runner, fs::copy_options::recursive);
    }
    mounts.push_back({group_agent_runner, "/app/src", false});

    // Additional mounts
    if (group.container_config) {
        auto validated = validate_additional_mounts(group.container_config->additional_mounts, group.name, is_main);
        mounts.insert(mounts.end(), validated.begin(), validated.end());
    }

    return mounts;
}

static std::vector<std::string> build_container_args(
    const std::vector<VolumeMount>& mounts,
    const std::string& container_name)
{
    std::vector<std::string> args = {"run", "-i", "--rm", "--name", container_name};
    const auto& cfg = config();

    // Timezone
    args.push_back("-e");
    args.push_back("TZ=" + cfg.timezone);

    // Host gateway args
    auto gw_args = host_gateway_args();
    args.insert(args.end(), gw_args.begin(), gw_args.end());

    // Run as host user
    uid_t uid = getuid();
    gid_t gid = getgid();
    if (uid != 0 && uid != 1000) {
        args.push_back("--user");
        args.push_back(std::to_string(uid) + ":" + std::to_string(gid));
        args.push_back("-e");
        args.push_back("HOME=/home/node");
    }

    // Volume mounts
    for (const auto& mount : mounts) {
        if (mount.readonly) {
            auto ro_args = readonly_mount_args(mount.host_path, mount.container_path);
            args.insert(args.end(), ro_args.begin(), ro_args.end());
        } else {
            args.push_back("-v");
            args.push_back(mount.host_path + ":" + mount.container_path);
        }
    }

    args.push_back(cfg.container_image);
    return args;
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

    auto mounts = build_volume_mounts(group, input.is_main);
    std::string safe_name = group.folder;
    for (auto& c : safe_name) {
        if (!std::isalnum(c) && c != '-') c = '-';
    }
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string container_name = "nanoclaw-" + safe_name + "-" + std::to_string(now_ms);

    auto container_args = build_container_args(mounts, container_name);

    logger()->info("Spawning container agent: group={}, container={}, mounts={}",
        group.name, container_name, mounts.size());

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

    // Create pipes for stdin/stdout/stderr
    int stdin_pipe[2], stdout_pipe[2], stderr_pipe[2];
    if (pipe(stdin_pipe) < 0 || pipe(stdout_pipe) < 0 || pipe(stderr_pipe) < 0) {
        return {"error", std::nullopt, std::nullopt, "Failed to create pipes"};
    }

    pid_t pid = fork();
    if (pid < 0) {
        return {"error", std::nullopt, std::nullopt, "Failed to fork"};
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
        argv.push_back(CONTAINER_RUNTIME_BIN.c_str());
        for (const auto& arg : container_args) {
            argv.push_back(arg.c_str());
        }
        argv.push_back(nullptr);

        execvp(CONTAINER_RUNTIME_BIN.c_str(), const_cast<char* const*>(argv.data()));
        _exit(127);
    }

    // Parent process
    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    on_process(pid, container_name);

    // Write input to stdin and close
    write(stdin_pipe[1], input_str.c_str(), input_str.size());
    close(stdin_pipe[1]);

    // Read stdout and stderr
    std::string stdout_buf, stderr_buf;
    std::string parse_buffer;
    std::optional<std::string> new_session_id;
    bool had_streaming_output = false;

    // Use select() to read from both pipes
    fd_set read_fds;
    int max_fd = std::max(stdout_pipe[0], stderr_pipe[0]);
    bool stdout_open = true, stderr_open = true;

    while (stdout_open || stderr_open) {
        FD_ZERO(&read_fds);
        if (stdout_open) FD_SET(stdout_pipe[0], &read_fds);
        if (stderr_open) FD_SET(stderr_pipe[0], &read_fds);

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int ret = select(max_fd + 1, &read_fds, nullptr, nullptr, &tv);
        if (ret < 0) break;

        if (stdout_open && FD_ISSET(stdout_pipe[0], &read_fds)) {
            char buf[4096];
            ssize_t n = read(stdout_pipe[0], buf, sizeof(buf));
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

        if (stderr_open && FD_ISSET(stderr_pipe[0], &read_fds)) {
            char buf[4096];
            ssize_t n = read(stderr_pipe[0], buf, sizeof(buf));
            if (n <= 0) {
                stderr_open = false;
            } else {
                stderr_buf.append(buf, n);
            }
        }
    }

    close(stdout_pipe[0]);
    close(stderr_pipe[0]);

    // Wait for child
    int status = 0;
    waitpid(pid, &status, 0);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    // Write log
    auto ts = now_iso();
    for (auto& c : ts) { if (c == ':' || c == '.') c = '-'; }
    auto log_file = (logs_dir / ("container-" + ts + ".log")).string();
    {
        std::ofstream log(log_file);
        log << "=== Container Run Log ===\n"
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
        logger()->error("Container exited with error: group={}, code={}, duration={}ms", group.name, exit_code, duration);
        return {"error", std::nullopt, std::nullopt,
            "Container exited with code " + std::to_string(exit_code) + ": " + stderr_buf.substr(0, std::min(stderr_buf.size(), size_t(200)))};
    }

    // Streaming mode
    if (on_output) {
        logger()->info("Container completed (streaming mode): group={}, duration={}ms", group.name, duration);
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

    return {"error", std::nullopt, std::nullopt, "Failed to parse container output"};
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
