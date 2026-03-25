#pragma once

#include "types.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace nanoclaw {

/**
 * Initialize the SQLite database, create schema, and run migrations.
 */
void init_database();

/**
 * Store chat metadata only (no message content).
 */
void store_chat_metadata(
    const std::string& chat_jid,
    const std::string& timestamp,
    const std::string& name = "",
    const std::string& channel = "",
    std::optional<bool> is_group = std::nullopt);

/**
 * Update chat name without changing timestamp.
 */
void update_chat_name(const std::string& chat_jid, const std::string& name);

/**
 * Get all known chats ordered by most recent activity.
 */
std::vector<ChatInfo> get_all_chats();

/**
 * Get last group metadata sync timestamp.
 */
std::optional<std::string> get_last_group_sync();

/**
 * Record that group metadata was synced.
 */
void set_last_group_sync();

/**
 * Store a message with full content.
 */
void store_message(const NewMessage& msg);

/**
 * Store a message directly (same as store_message, for compatibility).
 */
void store_message_direct(const NewMessage& msg);

struct GetNewMessagesResult {
    std::vector<NewMessage> messages;
    std::string new_timestamp;
};

/**
 * Get new messages from multiple JIDs since a given timestamp.
 */
GetNewMessagesResult get_new_messages(
    const std::vector<std::string>& jids,
    const std::string& last_timestamp,
    const std::string& bot_prefix,
    int limit = 200);

/**
 * Get messages for a single chat since a given timestamp.
 */
std::vector<NewMessage> get_messages_since(
    const std::string& chat_jid,
    const std::string& since_timestamp,
    const std::string& bot_prefix,
    int limit = 200);

// --- Task operations ---
void create_task(const ScheduledTask& task);
std::optional<ScheduledTask> get_task_by_id(const std::string& id);
std::vector<ScheduledTask> get_tasks_for_group(const std::string& group_folder);
std::vector<ScheduledTask> get_all_tasks();

struct TaskUpdates {
    std::optional<std::string> prompt;
    std::optional<std::string> schedule_type;
    std::optional<std::string> schedule_value;
    std::optional<std::string> next_run;
    std::optional<std::string> status;
};

void update_task(const std::string& id, const TaskUpdates& updates);
void delete_task(const std::string& id);
std::vector<ScheduledTask> get_due_tasks();
void update_task_after_run(const std::string& id, const std::optional<std::string>& next_run, const std::string& last_result);
void log_task_run(const TaskRunLog& log);

// --- Router state ---
std::optional<std::string> get_router_state(const std::string& key);
void set_router_state(const std::string& key, const std::string& value);

// --- Sessions ---
std::optional<std::string> get_session(const std::string& group_folder);
void set_session(const std::string& group_folder, const std::string& session_id);
std::map<std::string, std::string> get_all_sessions();

// --- Registered groups ---
struct RegisteredGroupRow {
    std::string jid;
    RegisteredGroup group;
};

std::optional<RegisteredGroupRow> get_registered_group(const std::string& jid);
void set_registered_group(const std::string& jid, const RegisteredGroup& group);
std::map<std::string, RegisteredGroup> get_all_registered_groups();

} // namespace nanoclaw
