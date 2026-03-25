#include "ipc.h"
#include "config.h"
#include "db.h"
#include "group_folder.h"
#include "logger.h"
#include "platform.h"
#include "timezone.h"

#include <croncpp.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace nanoclaw {

namespace fs = std::filesystem;
using json = nlohmann::json;

static bool ipc_watcher_running = false;

static void process_task_ipc(
    const json& data,
    const std::string& source_group,
    bool is_main,
    IpcDeps& deps)
{
    auto registered_groups = deps.registered_groups();
    std::string type = data.value("type", "");

    if (type == "schedule_task") {
        auto prompt = data.value("prompt", "");
        auto schedule_type = data.value("schedule_type", "");
        auto schedule_value = data.value("schedule_value", "");
        auto target_jid = data.value("targetJid", "");

        if (prompt.empty() || schedule_type.empty() || schedule_value.empty() || target_jid.empty()) return;

        auto it = registered_groups.find(target_jid);
        if (it == registered_groups.end()) {
            logger()->warn("Cannot schedule task: target group not registered: {}", target_jid);
            return;
        }

        auto target_folder = it->second.folder;
        if (!is_main && target_folder != source_group) {
            logger()->warn("Unauthorized schedule_task attempt: {} -> {}", source_group, target_folder);
            return;
        }

        std::optional<std::string> next_run;
        if (schedule_type == "cron") {
            try {
                auto cron = cron::make_cron(schedule_value);
                auto next = cron::cron_next(cron, std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
                struct tm tm_utc;
                platform::gmtime_safe(&next, &tm_utc);
                char buf[64];
                strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.000Z", &tm_utc);
                next_run = std::string(buf);
            } catch (...) {
                logger()->warn("Invalid cron expression: {}", schedule_value);
                return;
            }
        } else if (schedule_type == "interval") {
            int64_t ms = 0;
            try { ms = std::stoll(schedule_value); } catch (...) {}
            if (ms <= 0) return;
            auto next_time = std::chrono::system_clock::now() + std::chrono::milliseconds(ms);
            auto t = std::chrono::system_clock::to_time_t(next_time);
            struct tm tm_utc;
            platform::gmtime_safe(&t, &tm_utc);
            char buf[64];
            strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.000Z", &tm_utc);
            next_run = std::string(buf);
        } else if (schedule_type == "once") {
            next_run = schedule_value;
        }

        auto task_id = data.value("taskId", "task-" + std::to_string(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()));
        auto context_mode = data.value("context_mode", "isolated");
        if (context_mode != "group" && context_mode != "isolated") context_mode = "isolated";

        ScheduledTask task;
        task.id = task_id;
        task.group_folder = target_folder;
        task.chat_jid = target_jid;
        task.prompt = prompt;
        task.schedule_type = schedule_type;
        task.schedule_value = schedule_value;
        task.context_mode = context_mode;
        task.next_run = next_run;
        task.status = "active";
        task.created_at = now_iso();
        create_task(task);
        logger()->info("Task created via IPC: id={}, source={}, target={}", task_id, source_group, target_folder);
        deps.on_tasks_changed();

    } else if (type == "pause_task" || type == "resume_task" || type == "cancel_task") {
        auto task_id = data.value("taskId", "");
        if (task_id.empty()) return;

        auto task = get_task_by_id(task_id);
        if (!task || (!is_main && task->group_folder != source_group)) {
            logger()->warn("Unauthorized {} attempt: task={}, source={}", type, task_id, source_group);
            return;
        }

        if (type == "pause_task") {
            TaskUpdates upd; upd.status = "paused";
            update_task(task_id, upd);
        } else if (type == "resume_task") {
            TaskUpdates upd; upd.status = "active";
            update_task(task_id, upd);
        } else {
            delete_task(task_id);
        }
        logger()->info("Task {} via IPC: id={}, source={}", type, task_id, source_group);
        deps.on_tasks_changed();

    } else if (type == "update_task") {
        auto task_id = data.value("taskId", "");
        if (task_id.empty()) return;

        auto task = get_task_by_id(task_id);
        if (!task) return;
        if (!is_main && task->group_folder != source_group) return;

        TaskUpdates updates;
        if (data.contains("prompt")) updates.prompt = data["prompt"].get<std::string>();
        if (data.contains("schedule_type")) updates.schedule_type = data["schedule_type"].get<std::string>();
        if (data.contains("schedule_value")) updates.schedule_value = data["schedule_value"].get<std::string>();

        // Recompute next_run if schedule changed
        if (updates.schedule_type || updates.schedule_value) {
            ScheduledTask updated_task = *task;
            if (updates.schedule_type) updated_task.schedule_type = *updates.schedule_type;
            if (updates.schedule_value) updated_task.schedule_value = *updates.schedule_value;

            if (updated_task.schedule_type == "cron") {
                try {
                    auto cron = cron::make_cron(updated_task.schedule_value);
                    auto next = cron::cron_next(cron, std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
                    struct tm tm_utc;
                    platform::gmtime_safe(&next, &tm_utc);
                    char buf[64];
                    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.000Z", &tm_utc);
                    updates.next_run = std::string(buf);
                } catch (...) {}
            } else if (updated_task.schedule_type == "interval") {
                int64_t ms = 0;
                try { ms = std::stoll(updated_task.schedule_value); } catch (...) {}
                if (ms > 0) {
                    auto next_time = std::chrono::system_clock::now() + std::chrono::milliseconds(ms);
                    auto t = std::chrono::system_clock::to_time_t(next_time);
                    struct tm tm_utc;
                    platform::gmtime_safe(&t, &tm_utc);
                    char buf[64];
                    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.000Z", &tm_utc);
                    updates.next_run = std::string(buf);
                }
            }
        }

        update_task(task_id, updates);
        logger()->info("Task updated via IPC: id={}, source={}", task_id, source_group);
        deps.on_tasks_changed();

    } else if (type == "refresh_groups") {
        if (!is_main) {
            logger()->warn("Unauthorized refresh_groups from {}", source_group);
            return;
        }
        logger()->info("Group metadata refresh requested via IPC");
        deps.sync_groups(true);
        auto available = deps.get_available_groups();
        auto groups = deps.registered_groups();
        std::set<std::string> jids;
        for (const auto& [jid, _] : groups) jids.insert(jid);
        deps.write_groups_snapshot(source_group, true, available, jids);

    } else if (type == "register_group") {
        if (!is_main) {
            logger()->warn("Unauthorized register_group from {}", source_group);
            return;
        }
        auto jid = data.value("jid", "");
        auto name = data.value("name", "");
        auto folder = data.value("folder", "");
        auto trigger = data.value("trigger", "");
        if (jid.empty() || name.empty() || folder.empty() || trigger.empty()) return;

        if (!is_valid_group_folder(folder)) {
            logger()->warn("Invalid register_group folder: {}", folder);
            return;
        }

        RegisteredGroup group;
        group.name = name;
        group.folder = folder;
        group.trigger = trigger;
        group.added_at = now_iso();
        if (data.contains("requiresTrigger")) group.requires_trigger = data["requiresTrigger"].get<bool>();

        deps.register_group(jid, group);

    } else {
        logger()->warn("Unknown IPC task type: {}", type);
    }
}

void start_ipc_watcher(IpcDeps deps) {
    if (ipc_watcher_running) {
        logger()->debug("IPC watcher already running");
        return;
    }
    ipc_watcher_running = true;

    auto ipc_base_dir = fs::path(config().data_dir) / "ipc";
    fs::create_directories(ipc_base_dir);

    std::thread([deps = std::move(deps), ipc_base_dir]() mutable {
        while (true) {
            try {
                // Scan all group IPC directories
                std::vector<std::string> group_folders;
                for (const auto& entry : fs::directory_iterator(ipc_base_dir)) {
                    if (entry.is_directory() && entry.path().filename() != "errors") {
                        group_folders.push_back(entry.path().filename().string());
                    }
                }

                auto registered_groups = deps.registered_groups();

                // Build folder->isMain lookup
                std::map<std::string, bool> folder_is_main;
                for (const auto& [jid, group] : registered_groups) {
                    if (group.is_main && *group.is_main) {
                        folder_is_main[group.folder] = true;
                    }
                }

                for (const auto& source_group : group_folders) {
                    bool is_main = folder_is_main.count(source_group) > 0;
                    auto messages_dir = ipc_base_dir / source_group / "messages";
                    auto tasks_dir = ipc_base_dir / source_group / "tasks";

                    // Process messages
                    if (fs::exists(messages_dir)) {
                        for (const auto& file_entry : fs::directory_iterator(messages_dir)) {
                            if (file_entry.path().extension() != ".json") continue;
                            try {
                                std::ifstream f(file_entry.path());
                                auto data = json::parse(f);
                                if (data.value("type", "") == "message" &&
                                    data.contains("chatJid") && data.contains("text")) {
                                    auto chat_jid = data["chatJid"].get<std::string>();
                                    auto text = data["text"].get<std::string>();

                                    auto target_it = registered_groups.find(chat_jid);
                                    if (is_main || (target_it != registered_groups.end() && target_it->second.folder == source_group)) {
                                        deps.send_message(chat_jid, text);
                                        logger()->info("IPC message sent: jid={}, source={}", chat_jid, source_group);
                                    } else {
                                        logger()->warn("Unauthorized IPC message: jid={}, source={}", chat_jid, source_group);
                                    }
                                }
                                fs::remove(file_entry.path());
                            } catch (const std::exception& e) {
                                logger()->error("Error processing IPC message {}: {}", file_entry.path().string(), e.what());
                                auto error_dir = ipc_base_dir / "errors";
                                fs::create_directories(error_dir);
                                fs::rename(file_entry.path(), error_dir / (source_group + "-" + file_entry.path().filename().string()));
                            }
                        }
                    }

                    // Process tasks
                    if (fs::exists(tasks_dir)) {
                        for (const auto& file_entry : fs::directory_iterator(tasks_dir)) {
                            if (file_entry.path().extension() != ".json") continue;
                            try {
                                std::ifstream f(file_entry.path());
                                auto data = json::parse(f);
                                process_task_ipc(data, source_group, is_main, deps);
                                fs::remove(file_entry.path());
                            } catch (const std::exception& e) {
                                logger()->error("Error processing IPC task {}: {}", file_entry.path().string(), e.what());
                                auto error_dir = ipc_base_dir / "errors";
                                fs::create_directories(error_dir);
                                fs::rename(file_entry.path(), error_dir / (source_group + "-" + file_entry.path().filename().string()));
                            }
                        }
                    }
                }
            } catch (const std::exception& e) {
                logger()->error("Error in IPC watcher: {}", e.what());
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(config().ipc_poll_interval));
        }
    }).detach();

    logger()->info("IPC watcher started (per-group namespaces)");
}

} // namespace nanoclaw
