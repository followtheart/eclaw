#include "group_queue.h"
#include "config.h"
#include "logger.h"
#include "timezone.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>

namespace nanoclaw {

namespace fs = std::filesystem;
using json = nlohmann::json;

GroupQueue::GroupQueue() {}

void GroupQueue::set_process_messages_fn(ProcessMessagesFn fn) {
    process_messages_fn_ = std::move(fn);
}

GroupQueue::GroupState& GroupQueue::get_group(const std::string& group_jid) {
    return groups_[group_jid]; // default-constructs if missing
}

void GroupQueue::enqueue_message_check(const std::string& group_jid) {
    if (shutting_down_) return;

    auto& state = get_group(group_jid);

    if (state.active) {
        state.pending_messages = true;
        logger()->debug("Container active, message queued: {}", group_jid);
        return;
    }

    if (active_count_ >= config().max_concurrent_containers) {
        state.pending_messages = true;
        if (std::find(waiting_groups_.begin(), waiting_groups_.end(), group_jid) == waiting_groups_.end()) {
            waiting_groups_.push_back(group_jid);
        }
        logger()->debug("At concurrency limit ({}), message queued: {}", active_count_, group_jid);
        return;
    }

    // Run in a detached thread (mimics Node.js async behavior)
    std::thread([this, group_jid]() {
        run_for_group(group_jid, "messages");
    }).detach();
}

void GroupQueue::enqueue_task(const std::string& group_jid, const std::string& task_id, std::function<void()> fn) {
    if (shutting_down_) return;

    auto& state = get_group(group_jid);

    // Prevent double-queuing
    if (state.running_task_id == task_id) return;
    for (const auto& t : state.pending_tasks) {
        if (t.id == task_id) return;
    }

    QueuedTask task{task_id, group_jid, std::move(fn)};

    if (state.active) {
        state.pending_tasks.push_back(std::move(task));
        if (state.idle_waiting) close_stdin(group_jid);
        return;
    }

    if (active_count_ >= config().max_concurrent_containers) {
        state.pending_tasks.push_back(std::move(task));
        if (std::find(waiting_groups_.begin(), waiting_groups_.end(), group_jid) == waiting_groups_.end()) {
            waiting_groups_.push_back(group_jid);
        }
        return;
    }

    std::thread([this, group_jid, task = std::move(task)]() mutable {
        run_task(group_jid, std::move(task));
    }).detach();
}

void GroupQueue::register_process(const std::string& group_jid, pid_t pid, const std::string& container_name, const std::string& group_folder) {
    auto& state = get_group(group_jid);
    state.pid = pid;
    state.container_name = container_name;
    if (!group_folder.empty()) state.group_folder = group_folder;
}

void GroupQueue::notify_idle(const std::string& group_jid) {
    auto& state = get_group(group_jid);
    state.idle_waiting = true;
    if (!state.pending_tasks.empty()) {
        close_stdin(group_jid);
    }
}

bool GroupQueue::send_message(const std::string& group_jid, const std::string& text) {
    auto& state = get_group(group_jid);
    if (!state.active || state.group_folder.empty() || state.is_task_container) return false;
    state.idle_waiting = false;

    auto input_dir = fs::path(config().data_dir) / "ipc" / state.group_folder / "input";
    try {
        fs::create_directories(input_dir);
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        // Random suffix
        static std::mt19937 rng(std::random_device{}());
        static std::uniform_int_distribution<int> dist(0, 35);
        std::string suffix;
        for (int i = 0; i < 4; ++i) {
            int v = dist(rng);
            suffix += (v < 10 ? char('0' + v) : char('a' + v - 10));
        }

        std::string filename = std::to_string(now_ms) + "-" + suffix + ".json";
        auto filepath = input_dir / filename;
        auto temp_path = filepath.string() + ".tmp";

        json msg;
        msg["type"] = "message";
        msg["text"] = text;

        std::ofstream(temp_path) << msg.dump();
        fs::rename(temp_path, filepath);
        return true;
    } catch (...) {
        return false;
    }
}

void GroupQueue::close_stdin(const std::string& group_jid) {
    auto& state = get_group(group_jid);
    if (!state.active || state.group_folder.empty()) return;

    auto input_dir = fs::path(config().data_dir) / "ipc" / state.group_folder / "input";
    try {
        fs::create_directories(input_dir);
        auto close_file = input_dir / "_close";
        std::ofstream(close_file.string()) << "";
    } catch (...) {}
}

void GroupQueue::run_for_group(const std::string& group_jid, const std::string& reason) {
    auto& state = get_group(group_jid);
    state.active = true;
    state.idle_waiting = false;
    state.is_task_container = false;
    state.pending_messages = false;
    active_count_++;

    logger()->debug("Starting container: group={}, reason={}, active={}", group_jid, reason, active_count_);

    try {
        if (process_messages_fn_) {
            bool success = process_messages_fn_(group_jid);
            if (success) {
                state.retry_count = 0;
            } else {
                schedule_retry(group_jid, state);
            }
        }
    } catch (const std::exception& e) {
        logger()->error("Error processing messages for {}: {}", group_jid, e.what());
        schedule_retry(group_jid, state);
    }

    state.active = false;
    state.pid = 0;
    state.container_name.clear();
    state.group_folder.clear();
    active_count_--;
    drain_group(group_jid);
}

void GroupQueue::run_task(const std::string& group_jid, QueuedTask task) {
    auto& state = get_group(group_jid);
    state.active = true;
    state.idle_waiting = false;
    state.is_task_container = true;
    state.running_task_id = task.id;
    active_count_++;

    logger()->debug("Running queued task: group={}, task={}, active={}", group_jid, task.id, active_count_);

    try {
        task.fn();
    } catch (const std::exception& e) {
        logger()->error("Error running task {} for {}: {}", task.id, group_jid, e.what());
    }

    state.active = false;
    state.is_task_container = false;
    state.running_task_id.clear();
    state.pid = 0;
    state.container_name.clear();
    state.group_folder.clear();
    active_count_--;
    drain_group(group_jid);
}

void GroupQueue::schedule_retry(const std::string& group_jid, GroupState& state) {
    state.retry_count++;
    if (state.retry_count > MAX_RETRIES) {
        logger()->error("Max retries exceeded for {}, dropping messages", group_jid);
        state.retry_count = 0;
        return;
    }

    int delay_ms = BASE_RETRY_MS * static_cast<int>(std::pow(2, state.retry_count - 1));
    logger()->info("Scheduling retry for {} in {}ms (attempt {})", group_jid, delay_ms, state.retry_count);

    std::thread([this, group_jid, delay_ms]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        if (!shutting_down_) {
            enqueue_message_check(group_jid);
        }
    }).detach();
}

void GroupQueue::drain_group(const std::string& group_jid) {
    if (shutting_down_) return;

    auto& state = get_group(group_jid);

    // Tasks first
    if (!state.pending_tasks.empty()) {
        auto task = std::move(state.pending_tasks.front());
        state.pending_tasks.erase(state.pending_tasks.begin());
        std::thread([this, group_jid, task = std::move(task)]() mutable {
            run_task(group_jid, std::move(task));
        }).detach();
        return;
    }

    // Then pending messages
    if (state.pending_messages) {
        std::thread([this, group_jid]() {
            run_for_group(group_jid, "drain");
        }).detach();
        return;
    }

    drain_waiting();
}

void GroupQueue::drain_waiting() {
    while (!waiting_groups_.empty() && active_count_ < config().max_concurrent_containers) {
        auto next_jid = waiting_groups_.front();
        waiting_groups_.erase(waiting_groups_.begin());
        auto& state = get_group(next_jid);

        if (!state.pending_tasks.empty()) {
            auto task = std::move(state.pending_tasks.front());
            state.pending_tasks.erase(state.pending_tasks.begin());
            std::thread([this, jid = next_jid, task = std::move(task)]() mutable {
                run_task(jid, std::move(task));
            }).detach();
        } else if (state.pending_messages) {
            std::thread([this, jid = next_jid]() {
                run_for_group(jid, "drain");
            }).detach();
        }
    }
}

void GroupQueue::shutdown(int /*grace_period_ms*/) {
    shutting_down_ = true;

    std::vector<std::string> active_containers;
    for (const auto& [jid, state] : groups_) {
        if (state.pid > 0 && !state.container_name.empty()) {
            active_containers.push_back(state.container_name);
        }
    }

    logger()->info("GroupQueue shutting down (active={}, detached containers={})",
        active_count_, active_containers.size());
}

} // namespace nanoclaw
