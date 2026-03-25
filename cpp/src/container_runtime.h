#pragma once

#include <string>
#include <vector>

namespace nanoclaw {

/**
 * Ensure the agent-runner is built and ready to use.
 * Checks that node is available and agent-runner dist/index.js exists.
 * Throws on failure.
 */
void ensure_agent_runner_ready();

/**
 * Kill orphaned agent-runner processes from previous runs.
 */
void cleanup_orphan_processes();

} // namespace nanoclaw
