#pragma once

#include "group_queue.h"
#include "platform.h"
#include "types.h"

#include <functional>
#include <map>
#include <string>

namespace nanoclaw {

/**
 * Compute the next run time for a recurring task.
 * Anchored to task's scheduled time to prevent drift.
 */
std::optional<std::string> compute_next_run(const ScheduledTask& task);

struct SchedulerDependencies {
    std::function<std::map<std::string, RegisteredGroup>()> registered_groups;
    std::function<std::map<std::string, std::string>()> get_sessions;
    GroupQueue* queue = nullptr;
    std::function<void(const std::string& group_jid, pid_t pid, const std::string& container_name, const std::string& group_folder)> on_process;
    std::function<void(const std::string& jid, const std::string& text)> send_message;
};

/**
 * Start the scheduler loop. Polls every SCHEDULER_POLL_INTERVAL ms.
 */
void start_scheduler_loop(SchedulerDependencies deps);

} // namespace nanoclaw
