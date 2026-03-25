#pragma once

#include "types.h"

#include <functional>
#include <map>
#include <set>
#include <string>
#include <sys/types.h>
#include <vector>

namespace nanoclaw {

/**
 * Callback when container process is spawned.
 */
using OnProcessCallback = std::function<void(pid_t pid, const std::string& container_name)>;

/**
 * Callback for streaming output from container.
 */
using OnOutputCallback = std::function<void(const ContainerOutput& output)>;

/**
 * Run an agent in a container. Blocks until completion.
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
 * Write available groups snapshot for container to read.
 */
void write_groups_snapshot(
    const std::string& group_folder,
    bool is_main,
    const std::vector<AvailableGroup>& groups,
    const std::set<std::string>& registered_jids);

} // namespace nanoclaw
