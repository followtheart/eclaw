#pragma once

#include "container_runner.h"
#include "types.h"

#include <functional>
#include <map>
#include <set>
#include <string>

namespace nanoclaw {

struct IpcDeps {
    std::function<void(const std::string& jid, const std::string& text)> send_message;
    std::function<std::map<std::string, RegisteredGroup>()> registered_groups;
    std::function<void(const std::string& jid, const RegisteredGroup& group)> register_group;
    std::function<void(bool force)> sync_groups;
    std::function<std::vector<AvailableGroup>()> get_available_groups;
    std::function<void(const std::string&, bool, const std::vector<AvailableGroup>&, const std::set<std::string>&)> write_groups_snapshot;
    std::function<void()> on_tasks_changed;
};

/**
 * Start the IPC watcher loop. Polls per-group IPC directories.
 */
void start_ipc_watcher(IpcDeps deps);

} // namespace nanoclaw
