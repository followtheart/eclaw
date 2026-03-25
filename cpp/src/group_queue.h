#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <vector>

namespace nanoclaw {

/**
 * GroupQueue manages concurrent container execution with fair scheduling.
 * At most max_concurrent_containers run simultaneously.
 */
class GroupQueue {
public:
    using ProcessMessagesFn = std::function<bool(const std::string& group_jid)>;

    GroupQueue();

    void set_process_messages_fn(ProcessMessagesFn fn);
    void enqueue_message_check(const std::string& group_jid);
    void enqueue_task(const std::string& group_jid, const std::string& task_id, std::function<void()> fn);
    void register_process(const std::string& group_jid, pid_t pid, const std::string& container_name, const std::string& group_folder = "");
    void notify_idle(const std::string& group_jid);

    /**
     * Send a follow-up message to active container via IPC file.
     * Returns true if written, false if no active container.
     */
    bool send_message(const std::string& group_jid, const std::string& text);

    /**
     * Signal the active container to close.
     */
    void close_stdin(const std::string& group_jid);

    /**
     * Graceful shutdown.
     */
    void shutdown(int grace_period_ms);

private:
    struct QueuedTask {
        std::string id;
        std::string group_jid;
        std::function<void()> fn;
    };

    struct GroupState {
        bool active = false;
        bool idle_waiting = false;
        bool is_task_container = false;
        std::string running_task_id;
        bool pending_messages = false;
        std::vector<QueuedTask> pending_tasks;
        pid_t pid = 0;
        std::string container_name;
        std::string group_folder;
        int retry_count = 0;
    };

    static const int MAX_RETRIES = 5;
    static const int BASE_RETRY_MS = 5000;

    GroupState& get_group(const std::string& group_jid);
    void run_for_group(const std::string& group_jid, const std::string& reason);
    void run_task(const std::string& group_jid, QueuedTask task);
    void schedule_retry(const std::string& group_jid, GroupState& state);
    void drain_group(const std::string& group_jid);
    void drain_waiting();

    std::map<std::string, GroupState> groups_;
    int active_count_ = 0;
    std::vector<std::string> waiting_groups_;
    ProcessMessagesFn process_messages_fn_;
    bool shutting_down_ = false;
    std::mutex mutex_;
};

} // namespace nanoclaw
