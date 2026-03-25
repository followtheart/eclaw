#pragma once

#include "types.h"

#include <string>

namespace nanoclaw {

/**
 * Load the sender allowlist from external config file.
 * Returns default config if file doesn't exist.
 */
SenderAllowlistConfig load_sender_allowlist(const std::string& path_override = "");

/**
 * Check if a sender is allowed for a given chat.
 */
bool is_sender_allowed(const std::string& chat_jid, const std::string& sender, const SenderAllowlistConfig& cfg);

/**
 * Check if messages from denied senders should be dropped entirely.
 */
bool should_drop_message(const std::string& chat_jid, const SenderAllowlistConfig& cfg);

/**
 * Check if a trigger from this sender is allowed. Logs denial if enabled.
 */
bool is_trigger_allowed(const std::string& chat_jid, const std::string& sender, const SenderAllowlistConfig& cfg);

} // namespace nanoclaw
