#pragma once

#include "types.h"

#include <optional>
#include <string>
#include <vector>

namespace nanoclaw {

/**
 * Load the mount allowlist from external config location.
 * Returns nullopt if file doesn't exist or is invalid.
 * Cached in memory for process lifetime.
 */
std::optional<MountAllowlist> load_mount_allowlist();

/**
 * Validate a single additional mount against the allowlist.
 */
MountValidationResult validate_mount(const AdditionalMount& mount, bool is_main);

/**
 * Validate all additional mounts for a group.
 * Returns only validated (allowed) mounts as host paths.
 */
std::vector<ValidatedMount> validate_additional_mounts(
    const std::vector<AdditionalMount>& mounts,
    const std::string& group_name,
    bool is_main);

/**
 * Generate a template allowlist JSON string.
 */
std::string generate_allowlist_template();

} // namespace nanoclaw
