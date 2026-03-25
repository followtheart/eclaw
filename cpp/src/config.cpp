#include "config.h"
#include "env.h"
#include "platform.h"
#include "timezone.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <regex>

namespace nanoclaw {

namespace fs = std::filesystem;

static Config g_config;
static bool g_initialized = false;

static std::string get_env(const char* key, const std::string& fallback = "") {
    const char* val = std::getenv(key);
    return val ? std::string(val) : fallback;
}

static std::string get_home_dir() {
    return platform::get_home_dir();
}

static std::string escape_regex(const std::string& str) {
    static const std::regex special_chars(R"([.*+?^${}()|[\]\\])");
    return std::regex_replace(str, special_chars, R"(\$&)");
}

static int parse_int(const std::string& s, int fallback) {
    try {
        return std::stoi(s);
    } catch (...) {
        return fallback;
    }
}

void init_config() {
    auto env_config = read_env_file({
        "ASSISTANT_NAME",
        "ASSISTANT_HAS_OWN_NUMBER",
        "ONECLI_URL",
        "TZ",
    });

    std::string project_root = fs::current_path().string();
    std::string home_dir = get_home_dir();

    // Assistant name
    g_config.assistant_name = get_env("ASSISTANT_NAME",
        env_config.count("ASSISTANT_NAME") ? env_config["ASSISTANT_NAME"] : "Andy");

    // Assistant has own number
    std::string has_own = get_env("ASSISTANT_HAS_OWN_NUMBER",
        env_config.count("ASSISTANT_HAS_OWN_NUMBER") ? env_config["ASSISTANT_HAS_OWN_NUMBER"] : "false");
    g_config.assistant_has_own_number = (has_own == "true");

    // Paths
    g_config.mount_allowlist_path = (fs::path(home_dir) / ".config" / "nanoclaw" / "mount-allowlist.json").string();
    g_config.sender_allowlist_path = (fs::path(home_dir) / ".config" / "nanoclaw" / "sender-allowlist.json").string();
    g_config.store_dir = (fs::path(project_root) / "store").string();
    g_config.groups_dir = (fs::path(project_root) / "groups").string();
    g_config.data_dir = (fs::path(project_root) / "data").string();

    // Agent runner settings (replaces Docker container settings)
    auto default_runner = (fs::path(project_root) / "container" / "agent-runner" / "dist" / "index.js").string();
    g_config.agent_runner_path = get_env("AGENT_RUNNER_PATH", default_runner);
    g_config.agent_timeout = parse_int(get_env("AGENT_TIMEOUT", "1800000"), 1800000);
    g_config.onecli_url = get_env("ONECLI_URL",
        env_config.count("ONECLI_URL") ? env_config["ONECLI_URL"] : "http://localhost:10254");
    g_config.idle_timeout = parse_int(get_env("IDLE_TIMEOUT", "1800000"), 1800000);
    g_config.max_concurrent_agents = std::max(1,
        parse_int(get_env("MAX_CONCURRENT_AGENTS", "5"), 5));

    // Trigger pattern: ^@AssistantName\b (case-insensitive)
    std::string pattern = "^@" + escape_regex(g_config.assistant_name) + "\\b";
    g_config.trigger_pattern = std::regex(pattern, std::regex_constants::icase);

    // Timezone
    std::string tz_env = get_env("TZ");
    if (tz_env.empty() && env_config.count("TZ")) tz_env = env_config["TZ"];
    if (tz_env.empty() || !is_valid_timezone(tz_env)) tz_env = "UTC";
    g_config.timezone = tz_env;

    g_initialized = true;
}

const Config& config() {
    return g_config;
}

} // namespace nanoclaw
