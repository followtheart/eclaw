#pragma once

#include "types.h"

#include <memory>
#include <string>
#include <vector>

namespace nanoclaw {

/**
 * Escape XML special characters.
 */
std::string escape_xml(const std::string& s);

/**
 * Format messages into XML for the agent.
 */
std::string format_messages(const std::vector<NewMessage>& messages, const std::string& timezone);

/**
 * Strip <internal>...</internal> blocks from agent output.
 */
std::string strip_internal_tags(const std::string& text);

/**
 * Format outbound text (strips internal tags).
 */
std::string format_outbound(const std::string& raw_text);

/**
 * Find the channel that owns a given JID.
 */
Channel* find_channel(const std::vector<std::shared_ptr<Channel>>& channels, const std::string& jid);

} // namespace nanoclaw
