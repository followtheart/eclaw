#include "env.h"
#include "logger.h"

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace nanoclaw {

std::map<std::string, std::string> read_env_file(const std::vector<std::string>& keys) {
    namespace fs = std::filesystem;

    auto env_file = fs::current_path() / ".env";
    std::map<std::string, std::string> result;

    std::ifstream file(env_file);
    if (!file.is_open()) {
        logger()->debug(".env file not found, using defaults");
        return result;
    }

    std::set<std::string> wanted(keys.begin(), keys.end());
    std::string line;

    while (std::getline(file, line)) {
        // Trim whitespace
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        line = line.substr(start);

        // Skip comments
        if (line[0] == '#') continue;

        // Find '='
        auto eq_idx = line.find('=');
        if (eq_idx == std::string::npos) continue;

        std::string key = line.substr(0, eq_idx);
        // Trim key
        size_t key_end = key.find_last_not_of(" \t");
        if (key_end != std::string::npos) key = key.substr(0, key_end + 1);

        if (wanted.find(key) == wanted.end()) continue;

        std::string value = line.substr(eq_idx + 1);
        // Trim value
        start = value.find_first_not_of(" \t");
        if (start != std::string::npos) value = value.substr(start);
        size_t end = value.find_last_not_of(" \t\r\n");
        if (end != std::string::npos) value = value.substr(0, end + 1);

        // Remove surrounding quotes
        if (value.size() >= 2) {
            if ((value.front() == '"' && value.back() == '"') ||
                (value.front() == '\'' && value.back() == '\'')) {
                value = value.substr(1, value.size() - 2);
            }
        }

        if (!value.empty()) {
            result[key] = value;
        }
    }

    return result;
}

} // namespace nanoclaw
