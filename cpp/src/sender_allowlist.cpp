#include "sender_allowlist.h"
#include "config.h"
#include "logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace nanoclaw {

using json = nlohmann::json;

static SenderAllowlistConfig default_config() {
    SenderAllowlistConfig cfg;
    cfg.default_entry.allow = "*";
    cfg.default_entry.mode = "trigger";
    cfg.log_denied = true;
    return cfg;
}

static bool is_valid_entry(const json& entry) {
    if (!entry.is_object()) return false;
    if (!entry.contains("allow") || !entry.contains("mode")) return false;

    auto& allow = entry["allow"];
    if (!allow.is_string() && !allow.is_array()) return false;
    if (allow.is_string() && allow.get<std::string>() != "*") return false;
    if (allow.is_array()) {
        for (const auto& v : allow) {
            if (!v.is_string()) return false;
        }
    }

    auto mode = entry["mode"].get<std::string>();
    return mode == "trigger" || mode == "drop";
}

static ChatAllowlistEntry parse_entry(const json& entry) {
    ChatAllowlistEntry e;
    e.mode = entry["mode"].get<std::string>();

    if (entry["allow"].is_string()) {
        e.allow = "*";
    } else {
        e.allow = "list";
        for (const auto& v : entry["allow"]) {
            e.allow_list.push_back(v.get<std::string>());
        }
    }
    return e;
}

SenderAllowlistConfig load_sender_allowlist(const std::string& path_override) {
    std::string file_path = path_override.empty() ? config().sender_allowlist_path : path_override;

    std::ifstream file(file_path);
    if (!file.is_open()) return default_config();

    try {
        auto j = json::parse(file);

        if (!j.contains("default") || !is_valid_entry(j["default"])) {
            logger()->warn("sender-allowlist: invalid or missing default entry");
            return default_config();
        }

        SenderAllowlistConfig cfg;
        cfg.default_entry = parse_entry(j["default"]);
        cfg.log_denied = j.value("logDenied", true);

        if (j.contains("chats") && j["chats"].is_object()) {
            for (auto& [jid, entry] : j["chats"].items()) {
                if (is_valid_entry(entry)) {
                    cfg.chats[jid] = parse_entry(entry);
                } else {
                    logger()->warn("sender-allowlist: skipping invalid chat entry for {}", jid);
                }
            }
        }

        return cfg;
    } catch (...) {
        logger()->warn("sender-allowlist: invalid JSON in {}", file_path);
        return default_config();
    }
}

static const ChatAllowlistEntry& get_entry(const std::string& chat_jid, const SenderAllowlistConfig& cfg) {
    auto it = cfg.chats.find(chat_jid);
    return it != cfg.chats.end() ? it->second : cfg.default_entry;
}

bool is_sender_allowed(const std::string& chat_jid, const std::string& sender, const SenderAllowlistConfig& cfg) {
    const auto& entry = get_entry(chat_jid, cfg);
    if (entry.allow == "*") return true;
    return std::find(entry.allow_list.begin(), entry.allow_list.end(), sender) != entry.allow_list.end();
}

bool should_drop_message(const std::string& chat_jid, const SenderAllowlistConfig& cfg) {
    return get_entry(chat_jid, cfg).mode == "drop";
}

bool is_trigger_allowed(const std::string& chat_jid, const std::string& sender, const SenderAllowlistConfig& cfg) {
    bool allowed = is_sender_allowed(chat_jid, sender, cfg);
    if (!allowed && cfg.log_denied) {
        logger()->debug("sender-allowlist: trigger denied for sender {} in {}", sender, chat_jid);
    }
    return allowed;
}

} // namespace nanoclaw
