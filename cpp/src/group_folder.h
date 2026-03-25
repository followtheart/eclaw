#pragma once

#include <string>

namespace nanoclaw {

/**
 * Check if a folder name is valid for use as a group folder.
 * Pattern: /^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$/
 * Reserves "global" folder.
 */
bool is_valid_group_folder(const std::string& folder);

/**
 * Assert folder is valid, throw on failure.
 */
void assert_valid_group_folder(const std::string& folder);

/**
 * Resolve the absolute path for a group folder, ensuring it stays within GROUPS_DIR.
 */
std::string resolve_group_folder_path(const std::string& folder);

/**
 * Resolve the absolute path for a group's IPC directory.
 */
std::string resolve_group_ipc_path(const std::string& folder);

} // namespace nanoclaw
