#pragma once

#include <string>
#include <vector>

namespace nanoclaw {

/** The container runtime binary name. */
extern const std::string CONTAINER_RUNTIME_BIN;

/** CLI args for the container to resolve the host gateway. */
std::vector<std::string> host_gateway_args();

/** CLI args for a readonly bind mount. */
std::vector<std::string> readonly_mount_args(const std::string& host_path, const std::string& container_path);

/** Shell command to stop a container by name. */
std::string stop_container_cmd(const std::string& name);

/** Ensure the container runtime is running. Throws on failure. */
void ensure_container_runtime_running();

/** Kill orphaned NanoClaw containers from previous runs. */
void cleanup_orphans();

} // namespace nanoclaw
