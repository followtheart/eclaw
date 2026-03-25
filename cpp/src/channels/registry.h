#pragma once

#include "../types.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace nanoclaw {

struct ChannelOpts {
    OnInboundMessage on_message;
    OnChatMetadata on_chat_metadata;
    std::function<std::map<std::string, RegisteredGroup>()> registered_groups;
};

using ChannelFactory = std::function<std::shared_ptr<Channel>(const ChannelOpts&)>;

/**
 * Register a channel factory.
 */
void register_channel(const std::string& name, ChannelFactory factory);

/**
 * Get a channel factory by name.
 */
ChannelFactory get_channel_factory(const std::string& name);

/**
 * Get all registered channel names.
 */
std::vector<std::string> get_registered_channel_names();

} // namespace nanoclaw
