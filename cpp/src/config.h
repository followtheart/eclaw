#pragma once

#include <regex>
#include <string>

namespace nanoclaw {

/**
 * Global configuration - initialized once at startup.
 */
struct Config {
    std::string assistant_name;
    bool assistant_has_own_number = false;
    int poll_interval = 2000; // ms
    int scheduler_poll_interval = 60000; // ms

    std::string mount_allowlist_path;
    std::string sender_allowlist_path;
    std::string store_dir;
    std::string groups_dir;
    std::string data_dir;

    std::string container_image;
    int container_timeout = 1800000; // 30min
    int container_max_output_size = 10485760; // 10MB
    std::string onecli_url;
    int ipc_poll_interval = 1000; // ms
    int idle_timeout = 1800000; // 30min
    int max_concurrent_containers = 5;

    std::regex trigger_pattern;
    std::string timezone;
};

/**
 * Initialize global config. Must be called once at startup.
 */
void init_config();

/**
 * Get the global config instance (read-only after init).
 */
const Config& config();

} // namespace nanoclaw
