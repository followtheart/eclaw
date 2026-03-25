#include "db.h"
#include "config.h"
#include "group_folder.h"
#include "logger.h"
#include "timezone.h"

#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <memory>
#include <sstream>

namespace nanoclaw {

namespace fs = std::filesystem;
using json = nlohmann::json;

static std::unique_ptr<SQLite::Database> g_db;

static void create_schema(SQLite::Database& db) {
    db.exec(R"SQL(
        CREATE TABLE IF NOT EXISTS chats (
            jid TEXT PRIMARY KEY,
            name TEXT,
            last_message_time TEXT,
            channel TEXT,
            is_group INTEGER DEFAULT 0
        );
        CREATE TABLE IF NOT EXISTS messages (
            id TEXT,
            chat_jid TEXT,
            sender TEXT,
            sender_name TEXT,
            content TEXT,
            timestamp TEXT,
            is_from_me INTEGER,
            is_bot_message INTEGER DEFAULT 0,
            PRIMARY KEY (id, chat_jid),
            FOREIGN KEY (chat_jid) REFERENCES chats(jid)
        );
        CREATE INDEX IF NOT EXISTS idx_timestamp ON messages(timestamp);

        CREATE TABLE IF NOT EXISTS scheduled_tasks (
            id TEXT PRIMARY KEY,
            group_folder TEXT NOT NULL,
            chat_jid TEXT NOT NULL,
            prompt TEXT NOT NULL,
            schedule_type TEXT NOT NULL,
            schedule_value TEXT NOT NULL,
            next_run TEXT,
            last_run TEXT,
            last_result TEXT,
            status TEXT DEFAULT 'active',
            created_at TEXT NOT NULL
        );
        CREATE INDEX IF NOT EXISTS idx_next_run ON scheduled_tasks(next_run);
        CREATE INDEX IF NOT EXISTS idx_status ON scheduled_tasks(status);

        CREATE TABLE IF NOT EXISTS task_run_logs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            task_id TEXT NOT NULL,
            run_at TEXT NOT NULL,
            duration_ms INTEGER NOT NULL,
            status TEXT NOT NULL,
            result TEXT,
            error TEXT,
            FOREIGN KEY (task_id) REFERENCES scheduled_tasks(id)
        );
        CREATE INDEX IF NOT EXISTS idx_task_run_logs ON task_run_logs(task_id, run_at);

        CREATE TABLE IF NOT EXISTS router_state (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL
        );
        CREATE TABLE IF NOT EXISTS sessions (
            group_folder TEXT PRIMARY KEY,
            session_id TEXT NOT NULL
        );
        CREATE TABLE IF NOT EXISTS registered_groups (
            jid TEXT PRIMARY KEY,
            name TEXT NOT NULL,
            folder TEXT NOT NULL UNIQUE,
            trigger_pattern TEXT NOT NULL,
            added_at TEXT NOT NULL,
            container_config TEXT,
            requires_trigger INTEGER DEFAULT 1
        );
    )SQL");

    // Migrations - add columns if they don't exist
    auto try_alter = [&](const std::string& sql) {
        try { db.exec(sql); } catch (...) { /* column already exists */ }
    };

    try_alter("ALTER TABLE scheduled_tasks ADD COLUMN context_mode TEXT DEFAULT 'isolated'");
    try_alter("ALTER TABLE messages ADD COLUMN is_bot_message INTEGER DEFAULT 0");
    try_alter("ALTER TABLE registered_groups ADD COLUMN is_main INTEGER DEFAULT 0");
    try_alter("ALTER TABLE chats ADD COLUMN channel TEXT");
    try_alter("ALTER TABLE chats ADD COLUMN is_group INTEGER DEFAULT 0");

    // Backfill migrations
    try {
        db.exec("UPDATE registered_groups SET is_main = 1 WHERE folder = 'main'");
        db.exec("UPDATE chats SET channel = 'whatsapp', is_group = 1 WHERE jid LIKE '%@g.us'");
        db.exec("UPDATE chats SET channel = 'whatsapp', is_group = 0 WHERE jid LIKE '%@s.whatsapp.net'");
        db.exec("UPDATE chats SET channel = 'discord', is_group = 1 WHERE jid LIKE 'dc:%'");
        db.exec("UPDATE chats SET channel = 'telegram', is_group = 1 WHERE jid LIKE 'tg:%'");
    } catch (...) {}

    // Backfill bot messages
    try {
        auto& cfg = config();
        SQLite::Statement stmt(db, "UPDATE messages SET is_bot_message = 1 WHERE content LIKE ?");
        stmt.bind(1, cfg.assistant_name + ":%");
        stmt.exec();
    } catch (...) {}
}

void init_database() {
    auto db_path = fs::path(config().store_dir) / "messages.db";
    fs::create_directories(db_path.parent_path());

    g_db = std::make_unique<SQLite::Database>(
        db_path.string(),
        SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);

    create_schema(*g_db);

    logger()->info("Database initialized at {}", db_path.string());
}

void store_chat_metadata(
    const std::string& chat_jid,
    const std::string& timestamp,
    const std::string& name,
    const std::string& channel,
    std::optional<bool> is_group)
{
    int group_val = is_group.has_value() ? (*is_group ? 1 : 0) : -1;

    if (!name.empty()) {
        SQLite::Statement stmt(*g_db, R"SQL(
            INSERT INTO chats (jid, name, last_message_time, channel, is_group) VALUES (?, ?, ?, ?, ?)
            ON CONFLICT(jid) DO UPDATE SET
                name = excluded.name,
                last_message_time = MAX(last_message_time, excluded.last_message_time),
                channel = COALESCE(excluded.channel, channel),
                is_group = COALESCE(excluded.is_group, is_group)
        )SQL");
        stmt.bind(1, chat_jid);
        stmt.bind(2, name);
        stmt.bind(3, timestamp);
        if (channel.empty()) stmt.bind(4); else stmt.bind(4, channel);
        if (group_val == -1) stmt.bind(5); else stmt.bind(5, group_val);
        stmt.exec();
    } else {
        SQLite::Statement stmt(*g_db, R"SQL(
            INSERT INTO chats (jid, name, last_message_time, channel, is_group) VALUES (?, ?, ?, ?, ?)
            ON CONFLICT(jid) DO UPDATE SET
                last_message_time = MAX(last_message_time, excluded.last_message_time),
                channel = COALESCE(excluded.channel, channel),
                is_group = COALESCE(excluded.is_group, is_group)
        )SQL");
        stmt.bind(1, chat_jid);
        stmt.bind(2, chat_jid); // use jid as name
        stmt.bind(3, timestamp);
        if (channel.empty()) stmt.bind(4); else stmt.bind(4, channel);
        if (group_val == -1) stmt.bind(5); else stmt.bind(5, group_val);
        stmt.exec();
    }
}

void update_chat_name(const std::string& chat_jid, const std::string& name) {
    SQLite::Statement stmt(*g_db, R"SQL(
        INSERT INTO chats (jid, name, last_message_time) VALUES (?, ?, ?)
        ON CONFLICT(jid) DO UPDATE SET name = excluded.name
    )SQL");
    stmt.bind(1, chat_jid);
    stmt.bind(2, name);
    stmt.bind(3, now_iso());
    stmt.exec();
}

std::vector<ChatInfo> get_all_chats() {
    SQLite::Statement stmt(*g_db, R"SQL(
        SELECT jid, name, last_message_time, channel, is_group
        FROM chats ORDER BY last_message_time DESC
    )SQL");

    std::vector<ChatInfo> result;
    while (stmt.executeStep()) {
        ChatInfo info;
        info.jid = stmt.getColumn(0).getString();
        info.name = stmt.getColumn(1).getString();
        info.last_message_time = stmt.getColumn(2).getString();
        info.channel = stmt.getColumn(3).isNull() ? "" : stmt.getColumn(3).getString();
        info.is_group = stmt.getColumn(4).getInt();
        result.push_back(std::move(info));
    }
    return result;
}

std::optional<std::string> get_last_group_sync() {
    SQLite::Statement stmt(*g_db, "SELECT last_message_time FROM chats WHERE jid = '__group_sync__'");
    if (stmt.executeStep()) {
        return stmt.getColumn(0).getString();
    }
    return std::nullopt;
}

void set_last_group_sync() {
    SQLite::Statement stmt(*g_db,
        "INSERT OR REPLACE INTO chats (jid, name, last_message_time) VALUES ('__group_sync__', '__group_sync__', ?)");
    stmt.bind(1, now_iso());
    stmt.exec();
}

void store_message(const NewMessage& msg) {
    SQLite::Statement stmt(*g_db, R"SQL(
        INSERT OR REPLACE INTO messages (id, chat_jid, sender, sender_name, content, timestamp, is_from_me, is_bot_message)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
    )SQL");
    stmt.bind(1, msg.id);
    stmt.bind(2, msg.chat_jid);
    stmt.bind(3, msg.sender);
    stmt.bind(4, msg.sender_name);
    stmt.bind(5, msg.content);
    stmt.bind(6, msg.timestamp);
    stmt.bind(7, msg.is_from_me ? 1 : 0);
    stmt.bind(8, msg.is_bot_message ? 1 : 0);
    stmt.exec();
}

void store_message_direct(const NewMessage& msg) {
    store_message(msg);
}

GetNewMessagesResult get_new_messages(
    const std::vector<std::string>& jids,
    const std::string& last_timestamp,
    const std::string& bot_prefix,
    int limit)
{
    GetNewMessagesResult result;
    result.new_timestamp = last_timestamp;

    if (jids.empty()) return result;

    // Build placeholders
    std::string placeholders;
    for (size_t i = 0; i < jids.size(); ++i) {
        if (i > 0) placeholders += ",";
        placeholders += "?";
    }

    std::string sql = "SELECT * FROM ("
        "SELECT id, chat_jid, sender, sender_name, content, timestamp, is_from_me "
        "FROM messages "
        "WHERE timestamp > ? AND chat_jid IN (" + placeholders + ") "
        "AND is_bot_message = 0 AND content NOT LIKE ? "
        "AND content != '' AND content IS NOT NULL "
        "ORDER BY timestamp DESC LIMIT ?"
        ") ORDER BY timestamp";

    SQLite::Statement stmt(*g_db, sql);
    int idx = 1;
    stmt.bind(idx++, last_timestamp);
    for (const auto& jid : jids) {
        stmt.bind(idx++, jid);
    }
    stmt.bind(idx++, bot_prefix + ":%");
    stmt.bind(idx++, limit);

    while (stmt.executeStep()) {
        NewMessage msg;
        msg.id = stmt.getColumn(0).getString();
        msg.chat_jid = stmt.getColumn(1).getString();
        msg.sender = stmt.getColumn(2).getString();
        msg.sender_name = stmt.getColumn(3).getString();
        msg.content = stmt.getColumn(4).getString();
        msg.timestamp = stmt.getColumn(5).getString();
        msg.is_from_me = stmt.getColumn(6).getInt() != 0;
        result.messages.push_back(std::move(msg));

        if (result.messages.back().timestamp > result.new_timestamp) {
            result.new_timestamp = result.messages.back().timestamp;
        }
    }

    return result;
}

std::vector<NewMessage> get_messages_since(
    const std::string& chat_jid,
    const std::string& since_timestamp,
    const std::string& bot_prefix,
    int limit)
{
    std::string sql = R"SQL(
        SELECT * FROM (
            SELECT id, chat_jid, sender, sender_name, content, timestamp, is_from_me
            FROM messages
            WHERE chat_jid = ? AND timestamp > ?
                AND is_bot_message = 0 AND content NOT LIKE ?
                AND content != '' AND content IS NOT NULL
            ORDER BY timestamp DESC LIMIT ?
        ) ORDER BY timestamp
    )SQL";

    SQLite::Statement stmt(*g_db, sql);
    stmt.bind(1, chat_jid);
    stmt.bind(2, since_timestamp);
    stmt.bind(3, bot_prefix + ":%");
    stmt.bind(4, limit);

    std::vector<NewMessage> result;
    while (stmt.executeStep()) {
        NewMessage msg;
        msg.id = stmt.getColumn(0).getString();
        msg.chat_jid = stmt.getColumn(1).getString();
        msg.sender = stmt.getColumn(2).getString();
        msg.sender_name = stmt.getColumn(3).getString();
        msg.content = stmt.getColumn(4).getString();
        msg.timestamp = stmt.getColumn(5).getString();
        msg.is_from_me = stmt.getColumn(6).getInt() != 0;
        result.push_back(std::move(msg));
    }
    return result;
}

void create_task(const ScheduledTask& task) {
    SQLite::Statement stmt(*g_db, R"SQL(
        INSERT INTO scheduled_tasks (id, group_folder, chat_jid, prompt, schedule_type, schedule_value, context_mode, next_run, status, created_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )SQL");
    stmt.bind(1, task.id);
    stmt.bind(2, task.group_folder);
    stmt.bind(3, task.chat_jid);
    stmt.bind(4, task.prompt);
    stmt.bind(5, task.schedule_type);
    stmt.bind(6, task.schedule_value);
    stmt.bind(7, task.context_mode.empty() ? "isolated" : task.context_mode);
    if (task.next_run) stmt.bind(8, *task.next_run); else stmt.bind(8);
    stmt.bind(9, task.status);
    stmt.bind(10, task.created_at);
    stmt.exec();
}

static ScheduledTask row_to_task(SQLite::Statement& stmt) {
    ScheduledTask task;
    task.id = stmt.getColumn("id").getString();
    task.group_folder = stmt.getColumn("group_folder").getString();
    task.chat_jid = stmt.getColumn("chat_jid").getString();
    task.prompt = stmt.getColumn("prompt").getString();
    task.schedule_type = stmt.getColumn("schedule_type").getString();
    task.schedule_value = stmt.getColumn("schedule_value").getString();
    if (!stmt.getColumn("next_run").isNull()) task.next_run = stmt.getColumn("next_run").getString();
    if (!stmt.getColumn("last_run").isNull()) task.last_run = stmt.getColumn("last_run").getString();
    if (!stmt.getColumn("last_result").isNull()) task.last_result = stmt.getColumn("last_result").getString();
    task.status = stmt.getColumn("status").getString();
    task.created_at = stmt.getColumn("created_at").getString();
    // context_mode may not exist in old rows
    try {
        if (!stmt.getColumn("context_mode").isNull()) task.context_mode = stmt.getColumn("context_mode").getString();
        else task.context_mode = "isolated";
    } catch (...) { task.context_mode = "isolated"; }
    return task;
}

std::optional<ScheduledTask> get_task_by_id(const std::string& id) {
    SQLite::Statement stmt(*g_db, "SELECT * FROM scheduled_tasks WHERE id = ?");
    stmt.bind(1, id);
    if (stmt.executeStep()) {
        return row_to_task(stmt);
    }
    return std::nullopt;
}

std::vector<ScheduledTask> get_tasks_for_group(const std::string& group_folder) {
    SQLite::Statement stmt(*g_db, "SELECT * FROM scheduled_tasks WHERE group_folder = ? ORDER BY created_at DESC");
    stmt.bind(1, group_folder);
    std::vector<ScheduledTask> result;
    while (stmt.executeStep()) {
        result.push_back(row_to_task(stmt));
    }
    return result;
}

std::vector<ScheduledTask> get_all_tasks() {
    SQLite::Statement stmt(*g_db, "SELECT * FROM scheduled_tasks ORDER BY created_at DESC");
    std::vector<ScheduledTask> result;
    while (stmt.executeStep()) {
        result.push_back(row_to_task(stmt));
    }
    return result;
}

void update_task(const std::string& id, const TaskUpdates& updates) {
    std::vector<std::string> fields;
    std::vector<std::string> values;

    if (updates.prompt) { fields.push_back("prompt = ?"); values.push_back(*updates.prompt); }
    if (updates.schedule_type) { fields.push_back("schedule_type = ?"); values.push_back(*updates.schedule_type); }
    if (updates.schedule_value) { fields.push_back("schedule_value = ?"); values.push_back(*updates.schedule_value); }
    if (updates.next_run) { fields.push_back("next_run = ?"); values.push_back(*updates.next_run); }
    if (updates.status) { fields.push_back("status = ?"); values.push_back(*updates.status); }

    if (fields.empty()) return;

    std::string sql = "UPDATE scheduled_tasks SET ";
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) sql += ", ";
        sql += fields[i];
    }
    sql += " WHERE id = ?";

    SQLite::Statement stmt(*g_db, sql);
    int idx = 1;
    for (const auto& val : values) {
        stmt.bind(idx++, val);
    }
    stmt.bind(idx, id);
    stmt.exec();
}

void delete_task(const std::string& id) {
    g_db->exec("DELETE FROM task_run_logs WHERE task_id = '" + id + "'");
    g_db->exec("DELETE FROM scheduled_tasks WHERE id = '" + id + "'");
}

std::vector<ScheduledTask> get_due_tasks() {
    auto now = now_iso();
    SQLite::Statement stmt(*g_db, R"SQL(
        SELECT * FROM scheduled_tasks
        WHERE status = 'active' AND next_run IS NOT NULL AND next_run <= ?
        ORDER BY next_run
    )SQL");
    stmt.bind(1, now);
    std::vector<ScheduledTask> result;
    while (stmt.executeStep()) {
        result.push_back(row_to_task(stmt));
    }
    return result;
}

void update_task_after_run(const std::string& id, const std::optional<std::string>& next_run, const std::string& last_result) {
    auto now = now_iso();
    SQLite::Statement stmt(*g_db, R"SQL(
        UPDATE scheduled_tasks
        SET next_run = ?, last_run = ?, last_result = ?, status = CASE WHEN ? IS NULL THEN 'completed' ELSE status END
        WHERE id = ?
    )SQL");
    if (next_run) stmt.bind(1, *next_run); else stmt.bind(1);
    stmt.bind(2, now);
    stmt.bind(3, last_result);
    if (next_run) stmt.bind(4, *next_run); else stmt.bind(4);
    stmt.bind(5, id);
    stmt.exec();
}

void log_task_run(const TaskRunLog& log) {
    SQLite::Statement stmt(*g_db, R"SQL(
        INSERT INTO task_run_logs (task_id, run_at, duration_ms, status, result, error)
        VALUES (?, ?, ?, ?, ?, ?)
    )SQL");
    stmt.bind(1, log.task_id);
    stmt.bind(2, log.run_at);
    stmt.bind(3, static_cast<int64_t>(log.duration_ms));
    stmt.bind(4, log.status);
    if (log.result) stmt.bind(5, *log.result); else stmt.bind(5);
    if (log.error) stmt.bind(6, *log.error); else stmt.bind(6);
    stmt.exec();
}

// --- Router state ---

std::optional<std::string> get_router_state(const std::string& key) {
    SQLite::Statement stmt(*g_db, "SELECT value FROM router_state WHERE key = ?");
    stmt.bind(1, key);
    if (stmt.executeStep()) {
        return stmt.getColumn(0).getString();
    }
    return std::nullopt;
}

void set_router_state(const std::string& key, const std::string& value) {
    SQLite::Statement stmt(*g_db, "INSERT OR REPLACE INTO router_state (key, value) VALUES (?, ?)");
    stmt.bind(1, key);
    stmt.bind(2, value);
    stmt.exec();
}

// --- Sessions ---

std::optional<std::string> get_session(const std::string& group_folder) {
    SQLite::Statement stmt(*g_db, "SELECT session_id FROM sessions WHERE group_folder = ?");
    stmt.bind(1, group_folder);
    if (stmt.executeStep()) {
        return stmt.getColumn(0).getString();
    }
    return std::nullopt;
}

void set_session(const std::string& group_folder, const std::string& session_id) {
    SQLite::Statement stmt(*g_db, "INSERT OR REPLACE INTO sessions (group_folder, session_id) VALUES (?, ?)");
    stmt.bind(1, group_folder);
    stmt.bind(2, session_id);
    stmt.exec();
}

std::map<std::string, std::string> get_all_sessions() {
    SQLite::Statement stmt(*g_db, "SELECT group_folder, session_id FROM sessions");
    std::map<std::string, std::string> result;
    while (stmt.executeStep()) {
        result[stmt.getColumn(0).getString()] = stmt.getColumn(1).getString();
    }
    return result;
}

// --- Registered groups ---

std::optional<RegisteredGroupRow> get_registered_group(const std::string& jid) {
    SQLite::Statement stmt(*g_db, "SELECT * FROM registered_groups WHERE jid = ?");
    stmt.bind(1, jid);
    if (!stmt.executeStep()) return std::nullopt;

    std::string folder = stmt.getColumn("folder").getString();
    if (!is_valid_group_folder(folder)) {
        logger()->warn("Skipping registered group with invalid folder: jid={}, folder={}", jid, folder);
        return std::nullopt;
    }

    RegisteredGroupRow row;
    row.jid = jid;
    row.group.name = stmt.getColumn("name").getString();
    row.group.folder = folder;
    row.group.trigger = stmt.getColumn("trigger_pattern").getString();
    row.group.added_at = stmt.getColumn("added_at").getString();

    auto cc_col = stmt.getColumn("container_config");
    if (!cc_col.isNull()) {
        try {
            auto j = json::parse(cc_col.getString());
            ContainerConfig cc;
            if (j.contains("additionalMounts")) {
                for (const auto& m : j["additionalMounts"]) {
                    AdditionalMount am;
                    am.host_path = m.value("hostPath", "");
                    am.container_path = m.value("containerPath", "");
                    am.readonly = m.value("readonly", true);
                    cc.additional_mounts.push_back(am);
                }
            }
            if (j.contains("timeout")) cc.timeout = j["timeout"].get<int>();
            row.group.container_config = cc;
        } catch (...) {}
    }

    auto rt_col = stmt.getColumn("requires_trigger");
    if (!rt_col.isNull()) row.group.requires_trigger = (rt_col.getInt() == 1);

    try {
        auto im_col = stmt.getColumn("is_main");
        if (!im_col.isNull() && im_col.getInt() == 1) row.group.is_main = true;
    } catch (...) {}

    return row;
}

void set_registered_group(const std::string& jid, const RegisteredGroup& group) {
    if (!is_valid_group_folder(group.folder)) {
        throw std::runtime_error("Invalid group folder \"" + group.folder + "\" for JID " + jid);
    }

    std::string cc_json;
    if (group.container_config) {
        json j;
        json mounts = json::array();
        for (const auto& m : group.container_config->additional_mounts) {
            json mount;
            mount["hostPath"] = m.host_path;
            if (!m.container_path.empty()) mount["containerPath"] = m.container_path;
            mount["readonly"] = m.readonly;
            mounts.push_back(mount);
        }
        if (!mounts.empty()) j["additionalMounts"] = mounts;
        if (group.container_config->timeout != 300000) j["timeout"] = group.container_config->timeout;
        cc_json = j.dump();
    }

    SQLite::Statement stmt(*g_db, R"SQL(
        INSERT OR REPLACE INTO registered_groups (jid, name, folder, trigger_pattern, added_at, container_config, requires_trigger, is_main)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
    )SQL");
    stmt.bind(1, jid);
    stmt.bind(2, group.name);
    stmt.bind(3, group.folder);
    stmt.bind(4, group.trigger);
    stmt.bind(5, group.added_at);
    if (cc_json.empty()) stmt.bind(6); else stmt.bind(6, cc_json);
    stmt.bind(7, (!group.requires_trigger.has_value() || *group.requires_trigger) ? 1 : 0);
    stmt.bind(8, (group.is_main && *group.is_main) ? 1 : 0);
    stmt.exec();
}

std::map<std::string, RegisteredGroup> get_all_registered_groups() {
    SQLite::Statement stmt(*g_db, "SELECT * FROM registered_groups");
    std::map<std::string, RegisteredGroup> result;

    while (stmt.executeStep()) {
        std::string jid = stmt.getColumn("jid").getString();
        std::string folder = stmt.getColumn("folder").getString();

        if (!is_valid_group_folder(folder)) {
            logger()->warn("Skipping registered group with invalid folder: jid={}, folder={}", jid, folder);
            continue;
        }

        RegisteredGroup group;
        group.name = stmt.getColumn("name").getString();
        group.folder = folder;
        group.trigger = stmt.getColumn("trigger_pattern").getString();
        group.added_at = stmt.getColumn("added_at").getString();

        auto cc_col = stmt.getColumn("container_config");
        if (!cc_col.isNull()) {
            try {
                auto j = json::parse(cc_col.getString());
                ContainerConfig cc;
                if (j.contains("additionalMounts")) {
                    for (const auto& m : j["additionalMounts"]) {
                        AdditionalMount am;
                        am.host_path = m.value("hostPath", "");
                        am.container_path = m.value("containerPath", "");
                        am.readonly = m.value("readonly", true);
                        cc.additional_mounts.push_back(am);
                    }
                }
                if (j.contains("timeout")) cc.timeout = j["timeout"].get<int>();
                group.container_config = cc;
            } catch (...) {}
        }

        auto rt_col = stmt.getColumn("requires_trigger");
        if (!rt_col.isNull()) group.requires_trigger = (rt_col.getInt() == 1);

        try {
            auto im_col = stmt.getColumn("is_main");
            if (!im_col.isNull() && im_col.getInt() == 1) group.is_main = true;
        } catch (...) {}

        result[jid] = std::move(group);
    }
    return result;
}

} // namespace nanoclaw
