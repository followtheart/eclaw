/**
 * NanoClaw - Personal Claude Assistant (C++ rewrite)
 *
 * Main orchestrator: state management, message loop, agent invocation,
 * channel management, and subsystem startup.
 */
#include "channels/registry.h"
#include "config.h"
#include "container_runner.h"
#include "container_runtime.h"
#include "db.h"
#include "group_folder.h"
#include "group_queue.h"
#include "ipc.h"
#include "logger.h"
#include "remote_control.h"
#include "router.h"
#include "sender_allowlist.h"
#include "task_scheduler.h"
#include "timezone.h"
#include "types.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace nanoclaw {

using json = nlohmann::json;

// --- Global state ---
static std::string g_last_timestamp;
static std::map<std::string, std::string> g_sessions;
static std::map<std::string, RegisteredGroup> g_registered_groups;
static std::map<std::string, std::string> g_last_agent_timestamp;
static std::atomic<bool> g_message_loop_running{false};

static std::vector<std::shared_ptr<Channel>> g_channels;
static GroupQueue g_queue;

// --- State persistence ---
static void load_state() {
    auto ts = get_router_state("last_timestamp");
    g_last_timestamp = ts.value_or("");

    auto agent_ts = get_router_state("last_agent_timestamp");
    if (agent_ts) {
        try {
            auto j = json::parse(*agent_ts);
            for (auto& [key, value] : j.items()) {
                g_last_agent_timestamp[key] = value.get<std::string>();
            }
        } catch (...) {
            logger()->warn("Corrupted last_agent_timestamp in DB, resetting");
            g_last_agent_timestamp.clear();
        }
    }

    g_sessions = get_all_sessions();
    g_registered_groups = get_all_registered_groups();
    logger()->info("State loaded: {} groups", g_registered_groups.size());
}

static void save_state() {
    set_router_state("last_timestamp", g_last_timestamp);
    json agent_ts_json(g_last_agent_timestamp);
    set_router_state("last_agent_timestamp", agent_ts_json.dump());
}

// --- Group registration ---
static void register_group(const std::string& jid, const RegisteredGroup& group) {
    std::string group_dir;
    try {
        group_dir = resolve_group_folder_path(group.folder);
    } catch (const std::exception& e) {
        logger()->warn("Rejecting group registration: jid={}, folder={}, err={}", jid, group.folder, e.what());
        return;
    }

    g_registered_groups[jid] = group;
    set_registered_group(jid, group);

    std::filesystem::create_directories(std::filesystem::path(group_dir) / "logs");
    logger()->info("Group registered: jid={}, name={}, folder={}", jid, group.name, group.folder);
}

// --- Available groups ---
static std::vector<AvailableGroup> get_available_groups() {
    auto chats = get_all_chats();
    std::set<std::string> registered_jids;
    for (const auto& [jid, _] : g_registered_groups) {
        registered_jids.insert(jid);
    }

    std::vector<AvailableGroup> result;
    for (const auto& c : chats) {
        if (c.jid == "__group_sync__" || !c.is_group) continue;
        result.push_back({c.jid, c.name, c.last_message_time, registered_jids.count(c.jid) > 0});
    }
    return result;
}

// --- Agent execution ---
static std::string run_agent(
    const RegisteredGroup& group,
    const std::string& prompt,
    const std::string& chat_jid,
    std::function<void(const ContainerOutput&)> on_output)
{
    bool is_main = group.is_main.value_or(false);
    auto session_it = g_sessions.find(group.folder);
    std::optional<std::string> session_id;
    if (session_it != g_sessions.end()) session_id = session_it->second;

    // Update tasks snapshot
    auto tasks = get_all_tasks();
    std::vector<TaskSnapshot> snapshots;
    for (const auto& t : tasks) {
        snapshots.push_back({t.id, t.group_folder, t.prompt, t.schedule_type,
            t.schedule_value, t.status, t.next_run});
    }
    write_tasks_snapshot(group.folder, is_main, snapshots);

    // Update available groups snapshot
    auto available_groups = get_available_groups();
    std::set<std::string> reg_jids;
    for (const auto& [jid, _] : g_registered_groups) reg_jids.insert(jid);
    write_groups_snapshot(group.folder, is_main, available_groups, reg_jids);

    // Wrap on_output to track session ID
    auto wrapped_on_output = [&](const ContainerOutput& output) {
        if (output.new_session_id) {
            g_sessions[group.folder] = *output.new_session_id;
            set_session(group.folder, *output.new_session_id);
        }
        if (on_output) on_output(output);
    };

    try {
        ContainerInput input;
        input.prompt = prompt;
        input.session_id = session_id;
        input.group_folder = group.folder;
        input.chat_jid = chat_jid;
        input.is_main = is_main;
        input.assistant_name = config().assistant_name;

        auto output = run_container_agent(
            group, input,
            [&](pid_t pid, const std::string& container_name) {
                g_queue.register_process(chat_jid, pid, container_name, group.folder);
            },
            wrapped_on_output);

        if (output.new_session_id) {
            g_sessions[group.folder] = *output.new_session_id;
            set_session(group.folder, *output.new_session_id);
        }

        if (output.status == "error") {
            logger()->error("Container agent error: group={}, err={}", group.name, output.error.value_or("unknown"));
            return "error";
        }
        return "success";
    } catch (const std::exception& e) {
        logger()->error("Agent error: group={}, err={}", group.name, e.what());
        return "error";
    }
}

// --- Message processing ---
static bool process_group_messages(const std::string& chat_jid) {
    auto it = g_registered_groups.find(chat_jid);
    if (it == g_registered_groups.end()) return true;
    const auto& group = it->second;

    auto* channel = find_channel(g_channels, chat_jid);
    if (!channel) {
        logger()->warn("No channel owns JID {}, skipping messages", chat_jid);
        return true;
    }

    bool is_main = group.is_main.value_or(false);
    auto since_ts = g_last_agent_timestamp.count(chat_jid) ? g_last_agent_timestamp[chat_jid] : "";
    auto missed = get_messages_since(chat_jid, since_ts, config().assistant_name);
    if (missed.empty()) return true;

    // Check trigger requirement
    if (!is_main && group.requires_trigger.value_or(true)) {
        auto allowlist = load_sender_allowlist();
        bool has_trigger = false;
        for (const auto& m : missed) {
            std::string trimmed = m.content;
            size_t start = trimmed.find_first_not_of(" \t\n\r");
            if (start != std::string::npos) trimmed = trimmed.substr(start);
            if (std::regex_search(trimmed, config().trigger_pattern) &&
                (m.is_from_me || is_trigger_allowed(chat_jid, m.sender, allowlist))) {
                has_trigger = true;
                break;
            }
        }
        if (!has_trigger) return true;
    }

    auto prompt = format_messages(missed, config().timezone);

    // Advance cursor
    auto previous_cursor = g_last_agent_timestamp.count(chat_jid) ? g_last_agent_timestamp[chat_jid] : "";
    g_last_agent_timestamp[chat_jid] = missed.back().timestamp;
    save_state();

    logger()->info("Processing {} messages for group {}", missed.size(), group.name);

    channel->set_typing(chat_jid, true);
    bool had_error = false;
    bool output_sent = false;

    auto result = run_agent(group, prompt, chat_jid, [&](const ContainerOutput& output) {
        if (output.result) {
            auto text = strip_internal_tags(*output.result);
            if (!text.empty()) {
                channel->send_message(chat_jid, text);
                output_sent = true;
            }
        }
        if (output.status == "success") {
            g_queue.notify_idle(chat_jid);
        }
        if (output.status == "error") {
            had_error = true;
        }
    });

    channel->set_typing(chat_jid, false);

    if (result == "error" || had_error) {
        if (output_sent) {
            logger()->warn("Agent error after output sent for {}, skipping rollback", group.name);
            return true;
        }
        g_last_agent_timestamp[chat_jid] = previous_cursor;
        save_state();
        logger()->warn("Agent error for {}, rolled back cursor", group.name);
        return false;
    }

    return true;
}

// --- Message loop ---
static void start_message_loop() {
    if (g_message_loop_running.exchange(true)) {
        logger()->debug("Message loop already running");
        return;
    }

    logger()->info("NanoClaw running (trigger: @{})", config().assistant_name);

    while (true) {
        try {
            std::vector<std::string> jids;
            for (const auto& [jid, _] : g_registered_groups) jids.push_back(jid);

            auto result = get_new_messages(jids, g_last_timestamp, config().assistant_name);

            if (!result.messages.empty()) {
                logger()->info("{} new messages", result.messages.size());
                g_last_timestamp = result.new_timestamp;
                save_state();

                // Group by chat JID
                std::map<std::string, std::vector<NewMessage>> by_group;
                for (auto& msg : result.messages) {
                    by_group[msg.chat_jid].push_back(std::move(msg));
                }

                for (auto& [chat_jid, group_messages] : by_group) {
                    auto group_it = g_registered_groups.find(chat_jid);
                    if (group_it == g_registered_groups.end()) continue;
                    const auto& group = group_it->second;

                    auto* channel = find_channel(g_channels, chat_jid);
                    if (!channel) continue;

                    bool is_main = group.is_main.value_or(false);
                    bool needs_trigger = !is_main && group.requires_trigger.value_or(true);

                    if (needs_trigger) {
                        auto allowlist = load_sender_allowlist();
                        bool has_trigger = false;
                        for (const auto& m : group_messages) {
                            std::string trimmed = m.content;
                            size_t start = trimmed.find_first_not_of(" \t\n\r");
                            if (start != std::string::npos) trimmed = trimmed.substr(start);
                            if (std::regex_search(trimmed, config().trigger_pattern) &&
                                (m.is_from_me || is_trigger_allowed(chat_jid, m.sender, allowlist))) {
                                has_trigger = true;
                                break;
                            }
                        }
                        if (!has_trigger) continue;
                    }

                    // Pull all messages since lastAgentTimestamp
                    auto since = g_last_agent_timestamp.count(chat_jid) ? g_last_agent_timestamp[chat_jid] : "";
                    auto all_pending = get_messages_since(chat_jid, since, config().assistant_name);
                    auto& messages_to_send = all_pending.empty() ? group_messages : all_pending;
                    auto formatted = format_messages(messages_to_send, config().timezone);

                    if (g_queue.send_message(chat_jid, formatted)) {
                        logger()->debug("Piped {} messages to active container for {}", messages_to_send.size(), chat_jid);
                        g_last_agent_timestamp[chat_jid] = messages_to_send.back().timestamp;
                        save_state();
                        try { channel->set_typing(chat_jid, true); } catch (...) {}
                    } else {
                        g_queue.enqueue_message_check(chat_jid);
                    }
                }
            }
        } catch (const std::exception& e) {
            logger()->error("Error in message loop: {}", e.what());
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(config().poll_interval));
    }
}

// --- Startup recovery ---
static void recover_pending_messages() {
    for (const auto& [chat_jid, group] : g_registered_groups) {
        auto since = g_last_agent_timestamp.count(chat_jid) ? g_last_agent_timestamp[chat_jid] : "";
        auto pending = get_messages_since(chat_jid, since, config().assistant_name);
        if (!pending.empty()) {
            logger()->info("Recovery: {} unprocessed messages for {}", pending.size(), group.name);
            g_queue.enqueue_message_check(chat_jid);
        }
    }
}

// --- Remote control handler ---
static void handle_remote_control(
    const std::string& command,
    const std::string& chat_jid,
    const NewMessage& msg)
{
    auto it = g_registered_groups.find(chat_jid);
    if (it == g_registered_groups.end() || !it->second.is_main.value_or(false)) {
        logger()->warn("Remote control rejected: not main group: {}", chat_jid);
        return;
    }

    auto* channel = find_channel(g_channels, chat_jid);
    if (!channel) return;

    if (command == "/remote-control") {
        auto result = start_remote_control(msg.sender, chat_jid, std::filesystem::current_path().string());
        if (result.ok) {
            channel->send_message(chat_jid, result.url);
        } else {
            channel->send_message(chat_jid, "Remote Control failed: " + result.error);
        }
    } else {
        auto result = stop_remote_control();
        if (result.ok) {
            channel->send_message(chat_jid, "Remote Control session ended.");
        } else {
            channel->send_message(chat_jid, result.error);
        }
    }
}

} // namespace nanoclaw

// --- Signal handling ---
static std::atomic<bool> g_shutdown_requested{false};

static void signal_handler(int sig) {
    g_shutdown_requested = true;
}

// --- Main ---
int main(int argc, char* argv[]) {
    using namespace nanoclaw;

    // Initialize
    init_config();

    try {
        ensure_container_runtime_running();
        cleanup_orphans();
    } catch (const std::exception& e) {
        logger()->critical("Container runtime check failed: {}", e.what());
        return 1;
    }

    init_database();
    logger()->info("Database initialized");
    load_state();

    restore_remote_control();

    // Signal handlers
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);

    // Channel callbacks
    ChannelOpts channel_opts;
    channel_opts.on_message = [](const std::string& chat_jid, const NewMessage& msg) {
        // Remote control commands
        std::string trimmed = msg.content;
        size_t start = trimmed.find_first_not_of(" \t\n\r");
        if (start != std::string::npos) trimmed = trimmed.substr(start);
        size_t end = trimmed.find_last_not_of(" \t\n\r");
        if (end != std::string::npos) trimmed = trimmed.substr(0, end + 1);

        if (trimmed == "/remote-control" || trimmed == "/remote-control-end") {
            handle_remote_control(trimmed, chat_jid, msg);
            return;
        }

        // Sender allowlist drop mode
        if (!msg.is_from_me && !msg.is_bot_message && g_registered_groups.count(chat_jid)) {
            auto cfg = load_sender_allowlist();
            if (should_drop_message(chat_jid, cfg) && !is_sender_allowed(chat_jid, msg.sender, cfg)) {
                if (cfg.log_denied) {
                    logger()->debug("sender-allowlist: dropping message from {} in {}", msg.sender, chat_jid);
                }
                return;
            }
        }

        store_message(msg);
    };

    channel_opts.on_chat_metadata = [](const std::string& chat_jid, const std::string& timestamp,
                                       const std::string& name, const std::string& channel,
                                       std::optional<bool> is_group) {
        store_chat_metadata(chat_jid, timestamp, name, channel, is_group);
    };

    channel_opts.registered_groups = []() { return g_registered_groups; };

    // Create and connect channels
    for (const auto& channel_name : get_registered_channel_names()) {
        auto factory = get_channel_factory(channel_name);
        if (!factory) continue;
        auto channel = factory(channel_opts);
        if (!channel) {
            logger()->warn("Channel {} installed but credentials missing — skipping", channel_name);
            continue;
        }
        channel->connect();
        g_channels.push_back(channel);
    }

    if (g_channels.empty()) {
        logger()->critical("No channels connected");
        return 1;
    }

    // Start subsystems
    start_scheduler_loop({
        .registered_groups = []() { return g_registered_groups; },
        .get_sessions = []() { return g_sessions; },
        .queue = &g_queue,
        .on_process = [](const std::string& group_jid, pid_t pid, const std::string& cn, const std::string& gf) {
            g_queue.register_process(group_jid, pid, cn, gf);
        },
        .send_message = [](const std::string& jid, const std::string& raw_text) {
            auto* channel = find_channel(g_channels, jid);
            if (!channel) return;
            auto text = format_outbound(raw_text);
            if (!text.empty()) channel->send_message(jid, text);
        },
    });

    start_ipc_watcher({
        .send_message = [](const std::string& jid, const std::string& text) {
            auto* channel = find_channel(g_channels, jid);
            if (!channel) throw std::runtime_error("No channel for JID: " + jid);
            channel->send_message(jid, text);
        },
        .registered_groups = []() { return g_registered_groups; },
        .register_group = register_group,
        .sync_groups = [](bool force) {
            for (auto& ch : g_channels) {
                ch->sync_groups(force);
            }
        },
        .get_available_groups = get_available_groups,
        .write_groups_snapshot = [](const std::string& gf, bool im, const std::vector<AvailableGroup>& ag, const std::set<std::string>& rj) {
            write_groups_snapshot(gf, im, ag, rj);
        },
        .on_tasks_changed = []() {
            auto tasks = get_all_tasks();
            std::vector<TaskSnapshot> snapshots;
            for (const auto& t : tasks) {
                snapshots.push_back({t.id, t.group_folder, t.prompt, t.schedule_type,
                    t.schedule_value, t.status, t.next_run});
            }
            for (const auto& [jid, group] : g_registered_groups) {
                write_tasks_snapshot(group.folder, group.is_main.value_or(false), snapshots);
            }
        },
    });

    g_queue.set_process_messages_fn(process_group_messages);
    recover_pending_messages();

    // Start message loop in a thread
    std::thread message_thread(start_message_loop);

    // Wait for shutdown signal
    while (!g_shutdown_requested) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // Graceful shutdown
    logger()->info("Shutdown signal received");
    g_queue.shutdown(10000);
    for (auto& ch : g_channels) {
        ch->disconnect();
    }

    // Don't join message_thread as it's an infinite loop
    logger()->info("NanoClaw stopped");
    return 0;
}
