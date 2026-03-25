#include "task_scheduler.h"
#include "config.h"
#include "container_runner.h"
#include "db.h"
#include "group_folder.h"
#include "logger.h"
#include "platform.h"
#include "timezone.h"

#include <croncpp.h>

#include <chrono>
#include <cstdlib>
#include <thread>

namespace nanoclaw {

std::optional<std::string> compute_next_run(const ScheduledTask& task) {
    if (task.schedule_type == "once") return std::nullopt;

    auto now = std::chrono::system_clock::now();
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    if (task.schedule_type == "cron") {
        try {
            auto cron = cron::make_cron(task.schedule_value);
            auto next = cron::cron_next(cron, std::chrono::system_clock::to_time_t(now));
            auto next_tp = std::chrono::system_clock::from_time_t(next);
            auto next_ms = std::chrono::duration_cast<std::chrono::milliseconds>(next_tp.time_since_epoch()).count();

            // Format as ISO 8601
            struct tm tm_utc;
            time_t t = next;
            platform::gmtime_safe(&t, &tm_utc);
            char buf[64];
            strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.000Z", &tm_utc);
            return std::string(buf);
        } catch (...) {
            return std::nullopt;
        }
    }

    if (task.schedule_type == "interval") {
        int64_t ms = 0;
        try { ms = std::stoll(task.schedule_value); } catch (...) {}
        if (ms <= 0) {
            logger()->warn("Invalid interval value for task {}: {}", task.id, task.schedule_value);
            // Fallback to 1 minute
            auto next_time = now + std::chrono::minutes(1);
            auto t = std::chrono::system_clock::to_time_t(next_time);
            struct tm tm_utc;
            platform::gmtime_safe(&t, &tm_utc);
            char buf[64];
            strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.000Z", &tm_utc);
            return std::string(buf);
        }

        // Anchor to scheduled time to prevent drift
        // Parse next_run
        struct tm tm_next = {};
        if (task.next_run) {
            platform::strptime_portable(task.next_run->c_str(), "%Y-%m-%dT%H:%M:%S", &tm_next);
        }
        int64_t next = platform::timegm_portable(&tm_next) * 1000LL + ms;
        while (next <= now_ms) {
            next += ms;
        }

        time_t next_sec = next / 1000;
        struct tm tm_utc;
        platform::gmtime_safe(&next_sec, &tm_utc);
        char buf[64];
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.000Z", &tm_utc);
        return std::string(buf);
    }

    return std::nullopt;
}

static void run_scheduled_task(const ScheduledTask& task, SchedulerDependencies& deps) {
    auto start_time = std::chrono::steady_clock::now();

    std::string group_dir;
    try {
        group_dir = resolve_group_folder_path(task.group_folder);
    } catch (const std::exception& e) {
        TaskUpdates upd;
        upd.status = "paused";
        update_task(task.id, upd);
        logger()->error("Task {} has invalid group folder: {}", task.id, e.what());
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time).count();
        log_task_run({task.id, now_iso(), duration, "error", std::nullopt, std::string(e.what())});
        return;
    }

    std::filesystem::create_directories(group_dir);
    logger()->info("Running scheduled task: id={}, group={}", task.id, task.group_folder);

    auto groups = deps.registered_groups();
    const RegisteredGroup* group = nullptr;
    for (const auto& [jid, g] : groups) {
        if (g.folder == task.group_folder) {
            group = &g;
            break;
        }
    }

    if (!group) {
        logger()->error("Group not found for task {}: {}", task.id, task.group_folder);
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time).count();
        log_task_run({task.id, now_iso(), duration, "error", std::nullopt,
            "Group not found: " + task.group_folder});
        return;
    }

    bool is_main = group->is_main.value_or(false);

    // Update tasks snapshot
    auto all_tasks = get_all_tasks();
    std::vector<TaskSnapshot> snapshots;
    for (const auto& t : all_tasks) {
        snapshots.push_back({t.id, t.group_folder, t.prompt, t.schedule_type,
            t.schedule_value, t.status, t.next_run});
    }
    write_tasks_snapshot(task.group_folder, is_main, snapshots);

    std::optional<std::string> result;
    std::optional<std::string> error;

    auto sessions = deps.get_sessions();
    auto session_it = sessions.find(task.group_folder);
    std::optional<std::string> session_id;
    if (task.context_mode == "group" && session_it != sessions.end()) {
        session_id = session_it->second;
    }

    try {
        ContainerInput input;
        input.prompt = task.prompt;
        input.session_id = session_id;
        input.group_folder = task.group_folder;
        input.chat_jid = task.chat_jid;
        input.is_main = is_main;
        input.is_scheduled_task = true;
        input.assistant_name = config().assistant_name;

        auto output = run_container_agent(
            *group, input,
            [&](pid_t pid, const std::string& cn) {
                deps.on_process(task.chat_jid, pid, cn, task.group_folder);
            },
            [&](const ContainerOutput& streamed) {
                if (streamed.result) {
                    result = streamed.result;
                    deps.send_message(task.chat_jid, *streamed.result);
                }
                if (streamed.status == "success") {
                    deps.queue->notify_idle(task.chat_jid);
                }
                if (streamed.status == "error") {
                    error = streamed.error.value_or("Unknown error");
                }
            });

        if (output.status == "error") {
            error = output.error.value_or("Unknown error");
        } else if (output.result) {
            result = output.result;
        }

        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time).count();
        logger()->info("Task {} completed in {}ms", task.id, duration);
    } catch (const std::exception& e) {
        error = std::string(e.what());
        logger()->error("Task {} failed: {}", task.id, e.what());
    }

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time).count();

    log_task_run({task.id, now_iso(), duration,
        error ? "error" : "success", result, error});

    auto next_run = compute_next_run(task);
    std::string result_summary = error ? ("Error: " + *error)
        : result ? result->substr(0, 200)
        : "Completed";
    update_task_after_run(task.id, next_run, result_summary);
}

static bool scheduler_running = false;

void start_scheduler_loop(SchedulerDependencies deps) {
    if (scheduler_running) {
        logger()->debug("Scheduler loop already running, skipping");
        return;
    }
    scheduler_running = true;
    logger()->info("Scheduler loop started");

    std::thread([deps = std::move(deps)]() mutable {
        while (true) {
            try {
                auto due_tasks = get_due_tasks();
                if (!due_tasks.empty()) {
                    logger()->info("Found {} due tasks", due_tasks.size());
                }

                for (const auto& task : due_tasks) {
                    auto current = get_task_by_id(task.id);
                    if (!current || current->status != "active") continue;

                    auto task_copy = *current;
                    deps.queue->enqueue_task(current->chat_jid, current->id, [task_copy, &deps]() mutable {
                        run_scheduled_task(task_copy, deps);
                    });
                }
            } catch (const std::exception& e) {
                logger()->error("Error in scheduler loop: {}", e.what());
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(config().scheduler_poll_interval));
        }
    }).detach();
}

} // namespace nanoclaw
