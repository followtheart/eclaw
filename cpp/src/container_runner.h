#pragma once

#include "platform.h"
#include "types.h"

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace nanoclaw {

/**
 * Callback when agent process is spawned.
 */
using OnProcessCallback = std::function<void(pid_t pid, const std::string& process_name)>;

/**
 * Callback for streaming output from agent.
 */
using OnOutputCallback = std::function<void(const ContainerOutput& output)>;

/**
 * Run an agent directly on the host. Blocks until completion.
 */
ContainerOutput run_container_agent(
    const RegisteredGroup& group,
    const ContainerInput& input,
    OnProcessCallback on_process,
    OnOutputCallback on_output = nullptr);

/**
 * Write tasks snapshot to group's IPC directory.
 */
struct TaskSnapshot {
    std::string id;
    std::string group_folder;
    std::string prompt;
    std::string schedule_type;
    std::string schedule_value;
    std::string status;
    std::optional<std::string> next_run;
};

void write_tasks_snapshot(
    const std::string& group_folder,
    bool is_main,
    const std::vector<TaskSnapshot>& tasks);

/**
 * Write available groups snapshot for agent to read.
 */
void write_groups_snapshot(
    const std::string& group_folder,
    bool is_main,
    const std::vector<AvailableGroup>& groups,
    const std::set<std::string>& registered_jids);

} // namespace nanoclaw
