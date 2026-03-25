#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace nanoclaw {

struct AdditionalMount {
    std::string host_path;
    std::string container_path; // defaults to basename of host_path
    bool readonly = true;
};

struct AllowedRoot {
    std::string path;
    bool allow_read_write = false;
    std::string description;
};

struct MountAllowlist {
    std::vector<AllowedRoot> allowed_roots;
    std::vector<std::string> blocked_patterns;
    bool non_main_read_only = true;
};

struct ContainerConfig {
    std::vector<AdditionalMount> additional_mounts;
    int timeout = 300000; // 5 minutes default
};

struct RegisteredGroup {
    std::string name;
    std::string folder;
    std::string trigger;
    std::string added_at;
    std::optional<ContainerConfig> container_config;
    std::optional<bool> requires_trigger; // default: true for groups, false for solo
    std::optional<bool> is_main;
};

struct NewMessage {
    std::string id;
    std::string chat_jid;
    std::string sender;
    std::string sender_name;
    std::string content;
    std::string timestamp;
    bool is_from_me = false;
    bool is_bot_message = false;
};

struct ScheduledTask {
    std::string id;
    std::string group_folder;
    std::string chat_jid;
    std::string prompt;
    std::string schedule_type; // "cron" | "interval" | "once"
    std::string schedule_value;
    std::string context_mode; // "group" | "isolated"
    std::optional<std::string> next_run;
    std::optional<std::string> last_run;
    std::optional<std::string> last_result;
    std::string status; // "active" | "paused" | "completed"
    std::string created_at;
};

struct TaskRunLog {
    std::string task_id;
    std::string run_at;
    int64_t duration_ms = 0;
    std::string status; // "success" | "error"
    std::optional<std::string> result;
    std::optional<std::string> error;
};

struct ChatInfo {
    std::string jid;
    std::string name;
    std::string last_message_time;
    std::string channel;
    int is_group = 0;
};

struct AvailableGroup {
    std::string jid;
    std::string name;
    std::string last_activity;
    bool is_registered = false;
};

// Channel abstraction
class Channel {
public:
    virtual ~Channel() = default;
    virtual const std::string& get_name() const = 0;
    virtual void connect() = 0;
    virtual void send_message(const std::string& jid, const std::string& text) = 0;
    virtual bool is_connected() const = 0;
    virtual bool owns_jid(const std::string& jid) const = 0;
    virtual void disconnect() = 0;
    virtual void set_typing(const std::string& /*jid*/, bool /*is_typing*/) {}
    virtual void sync_groups(bool /*force*/) {}
};

// Callback types
using OnInboundMessage = std::function<void(const std::string& chat_jid, const NewMessage& msg)>;
using OnChatMetadata = std::function<void(
    const std::string& chat_jid,
    const std::string& timestamp,
    const std::string& name,
    const std::string& channel,
    std::optional<bool> is_group)>;

struct ContainerInput {
    std::string prompt;
    std::optional<std::string> session_id;
    std::string group_folder;
    std::string chat_jid;
    bool is_main = false;
    bool is_scheduled_task = false;
    std::string assistant_name;
};

struct ContainerOutput {
    std::string status; // "success" | "error"
    std::optional<std::string> result;
    std::optional<std::string> new_session_id;
    std::optional<std::string> error;
};

struct VolumeMount {
    std::string host_path;
    std::string container_path;
    bool readonly = false;
};

// Sender allowlist types
struct ChatAllowlistEntry {
    std::string allow; // "*" or JSON array string
    std::vector<std::string> allow_list; // parsed list if not "*"
    std::string mode; // "trigger" | "drop"
};

struct SenderAllowlistConfig {
    ChatAllowlistEntry default_entry;
    std::map<std::string, ChatAllowlistEntry> chats;
    bool log_denied = true;
};

// Mount validation result
struct MountValidationResult {
    bool allowed = false;
    std::string reason;
    std::string real_host_path;
    std::string resolved_container_path;
    bool effective_readonly = true;
};

} // namespace nanoclaw
