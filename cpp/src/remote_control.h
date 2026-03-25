#pragma once

#include <optional>
#include <string>

namespace nanoclaw {

struct RemoteControlSession {
    int pid = 0;
    std::string url;
    std::string started_by;
    std::string started_in_chat;
    std::string started_at;
};

/**
 * Restore remote control session from disk on startup.
 */
void restore_remote_control();

/**
 * Get the active remote control session, if any.
 */
std::optional<RemoteControlSession> get_active_session();

struct RemoteControlResult {
    bool ok = false;
    std::string url;    // if ok
    std::string error;  // if !ok
};

/**
 * Start a remote control session.
 */
RemoteControlResult start_remote_control(
    const std::string& sender,
    const std::string& chat_jid,
    const std::string& cwd);

/**
 * Stop the active remote control session.
 */
RemoteControlResult stop_remote_control();

} // namespace nanoclaw
