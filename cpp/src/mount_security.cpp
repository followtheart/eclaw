#include "mount_security.h"
#include "config.h"
#include "logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <pwd.h>
#include <set>
#include <unistd.h>

namespace nanoclaw {

namespace fs = std::filesystem;
using json = nlohmann::json;

static const std::vector<std::string> DEFAULT_BLOCKED_PATTERNS = {
    ".ssh", ".gnupg", ".gpg", ".aws", ".azure", ".gcloud",
    ".kube", ".docker", "credentials", ".env", ".netrc",
    ".npmrc", ".pypirc", "id_rsa", "id_ed25519", "private_key", ".secret"
};

static std::optional<MountAllowlist> g_cached_allowlist;
static bool g_load_attempted = false;

static std::string get_home_dir() {
    const char* home = std::getenv("HOME");
    if (home) return home;
    struct passwd* pw = getpwuid(getuid());
    if (pw) return pw->pw_dir;
    return "/tmp";
}

static std::string expand_path(const std::string& p) {
    if (p.size() >= 2 && p.substr(0, 2) == "~/") {
        return get_home_dir() + p.substr(1);
    }
    if (p == "~") return get_home_dir();
    return fs::absolute(p).string();
}

static std::string get_real_path(const std::string& p) {
    try {
        return fs::canonical(p).string();
    } catch (...) {
        return "";
    }
}

std::optional<MountAllowlist> load_mount_allowlist() {
    if (g_load_attempted) return g_cached_allowlist;
    g_load_attempted = true;

    const auto& path = config().mount_allowlist_path;

    if (!fs::exists(path)) {
        logger()->warn("Mount allowlist not found at {} - additional mounts will be BLOCKED", path);
        return std::nullopt;
    }

    try {
        std::ifstream file(path);
        auto j = json::parse(file);

        MountAllowlist allowlist;

        for (const auto& root : j["allowedRoots"]) {
            AllowedRoot ar;
            ar.path = root["path"].get<std::string>();
            ar.allow_read_write = root.value("allowReadWrite", false);
            ar.description = root.value("description", "");
            allowlist.allowed_roots.push_back(ar);
        }

        // Merge blocked patterns
        std::set<std::string> patterns(DEFAULT_BLOCKED_PATTERNS.begin(), DEFAULT_BLOCKED_PATTERNS.end());
        for (const auto& p : j["blockedPatterns"]) {
            patterns.insert(p.get<std::string>());
        }
        allowlist.blocked_patterns.assign(patterns.begin(), patterns.end());
        allowlist.non_main_read_only = j.value("nonMainReadOnly", true);

        g_cached_allowlist = allowlist;
        logger()->info("Mount allowlist loaded: {} roots, {} blocked patterns",
            allowlist.allowed_roots.size(), allowlist.blocked_patterns.size());
        return g_cached_allowlist;
    } catch (const std::exception& e) {
        logger()->error("Failed to load mount allowlist at {}: {}", path, e.what());
        return std::nullopt;
    }
}

static std::string matches_blocked_pattern(const std::string& real_path, const std::vector<std::string>& patterns) {
    for (const auto& pattern : patterns) {
        // Check path components
        fs::path p(real_path);
        for (const auto& part : p) {
            std::string s = part.string();
            if (s == pattern || s.find(pattern) != std::string::npos) {
                return pattern;
            }
        }
        if (real_path.find(pattern) != std::string::npos) {
            return pattern;
        }
    }
    return "";
}

static const AllowedRoot* find_allowed_root(const std::string& real_path, const std::vector<AllowedRoot>& roots) {
    for (const auto& root : roots) {
        auto expanded = expand_path(root.path);
        auto real_root = get_real_path(expanded);
        if (real_root.empty()) continue;

        auto rel = fs::relative(real_path, real_root);
        std::string rel_str = rel.string();
        if (!rel_str.empty() && rel_str.substr(0, 2) != ".." && !fs::path(rel_str).is_absolute()) {
            return &root;
        }
    }
    return nullptr;
}

MountValidationResult validate_mount(const AdditionalMount& mount, bool is_main) {
    auto allowlist = load_mount_allowlist();
    if (!allowlist) {
        return {false, "No mount allowlist configured at " + config().mount_allowlist_path};
    }

    std::string container_path = mount.container_path.empty()
        ? fs::path(mount.host_path).filename().string()
        : mount.container_path;

    // Validate container path
    if (container_path.find("..") != std::string::npos ||
        container_path.front() == '/' ||
        container_path.empty()) {
        return {false, "Invalid container path: \"" + container_path + "\""};
    }

    auto expanded = expand_path(mount.host_path);
    auto real_path = get_real_path(expanded);
    if (real_path.empty()) {
        return {false, "Host path does not exist: \"" + mount.host_path + "\""};
    }

    auto blocked = matches_blocked_pattern(real_path, allowlist->blocked_patterns);
    if (!blocked.empty()) {
        return {false, "Path matches blocked pattern \"" + blocked + "\": \"" + real_path + "\""};
    }

    auto* root = find_allowed_root(real_path, allowlist->allowed_roots);
    if (!root) {
        return {false, "Path \"" + real_path + "\" is not under any allowed root"};
    }

    bool effective_readonly = true;
    if (!mount.readonly) {
        if (!is_main && allowlist->non_main_read_only) {
            effective_readonly = true;
        } else if (!root->allow_read_write) {
            effective_readonly = true;
        } else {
            effective_readonly = false;
        }
    }

    MountValidationResult result;
    result.allowed = true;
    result.reason = "Allowed under root \"" + root->path + "\"";
    result.real_host_path = real_path;
    result.resolved_container_path = container_path;
    result.effective_readonly = effective_readonly;
    return result;
}

std::vector<VolumeMount> validate_additional_mounts(
    const std::vector<AdditionalMount>& mounts,
    const std::string& group_name,
    bool is_main)
{
    std::vector<VolumeMount> validated;
    for (const auto& mount : mounts) {
        auto result = validate_mount(mount, is_main);
        if (result.allowed) {
            validated.push_back({
                result.real_host_path,
                "/workspace/extra/" + result.resolved_container_path,
                result.effective_readonly
            });
        } else {
            logger()->warn("Additional mount REJECTED for group {}: {} (reason: {})",
                group_name, mount.host_path, result.reason);
        }
    }
    return validated;
}

std::string generate_allowlist_template() {
    json j;
    j["allowedRoots"] = json::array({
        {{"path", "~/projects"}, {"allowReadWrite", true}, {"description", "Development projects"}},
        {{"path", "~/repos"}, {"allowReadWrite", true}, {"description", "Git repositories"}},
        {{"path", "~/Documents/work"}, {"allowReadWrite", false}, {"description", "Work documents (read-only)"}}
    });
    j["blockedPatterns"] = json::array({"password", "secret", "token"});
    j["nonMainReadOnly"] = true;
    return j.dump(2);
}

} // namespace nanoclaw
