#include "group_folder.h"
#include "config.h"

#include <algorithm>
#include <filesystem>
#include <regex>
#include <set>
#include <stdexcept>

namespace nanoclaw {

namespace fs = std::filesystem;

static const std::regex GROUP_FOLDER_PATTERN("^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$");
static const std::set<std::string> RESERVED_FOLDERS = {"global"};

bool is_valid_group_folder(const std::string& folder) {
    if (folder.empty()) return false;
    if (folder != folder.substr(folder.find_first_not_of(" \t") == std::string::npos ? 0 : folder.find_first_not_of(" \t"))) return false;
    if (!std::regex_match(folder, GROUP_FOLDER_PATTERN)) return false;
    if (folder.find('/') != std::string::npos || folder.find('\\') != std::string::npos) return false;
    if (folder.find("..") != std::string::npos) return false;

    std::string lower = folder;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (RESERVED_FOLDERS.count(lower)) return false;

    return true;
}

void assert_valid_group_folder(const std::string& folder) {
    if (!is_valid_group_folder(folder)) {
        throw std::runtime_error("Invalid group folder \"" + folder + "\"");
    }
}

static void ensure_within_base(const std::string& base_dir, const std::string& resolved_path) {
    auto rel = fs::relative(resolved_path, base_dir);
    std::string rel_str = rel.string();
    if (rel_str.substr(0, 2) == ".." || fs::path(rel_str).is_absolute()) {
        throw std::runtime_error("Path escapes base directory: " + resolved_path);
    }
}

std::string resolve_group_folder_path(const std::string& folder) {
    assert_valid_group_folder(folder);
    auto group_path = fs::absolute(fs::path(config().groups_dir) / folder).string();
    ensure_within_base(config().groups_dir, group_path);
    return group_path;
}

std::string resolve_group_ipc_path(const std::string& folder) {
    assert_valid_group_folder(folder);
    auto ipc_base = fs::absolute(fs::path(config().data_dir) / "ipc").string();
    auto ipc_path = fs::absolute(fs::path(ipc_base) / folder).string();
    ensure_within_base(ipc_base, ipc_path);
    return ipc_path;
}

} // namespace nanoclaw
