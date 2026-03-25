#include "registry.h"

namespace nanoclaw {

static std::map<std::string, ChannelFactory>& get_registry() {
    static std::map<std::string, ChannelFactory> registry;
    return registry;
}

void register_channel(const std::string& name, ChannelFactory factory) {
    get_registry()[name] = std::move(factory);
}

ChannelFactory get_channel_factory(const std::string& name) {
    auto& registry = get_registry();
    auto it = registry.find(name);
    if (it != registry.end()) return it->second;
    return nullptr;
}

std::vector<std::string> get_registered_channel_names() {
    std::vector<std::string> names;
    for (const auto& [name, _] : get_registry()) {
        names.push_back(name);
    }
    return names;
}

} // namespace nanoclaw
