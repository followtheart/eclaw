#include "container_runtime.h"
#include "config.h"
#include "logger.h"
#include "platform.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <stdexcept>

namespace nanoclaw {

namespace fs = std::filesystem;

void ensure_agent_runner_ready() {
    const auto& cfg = config();

    // Check that node is available
    std::string cmd = platform::suppress_stderr("node --version");
    int ret = platform::system_command(cmd);
    if (ret != 0) {
        logger()->error("Node.js not found in PATH");
        fprintf(stderr, "\n"
            "╔════════════════════════════════════════════════════════════════╗\n"
            "║  FATAL: Node.js not found                                      ║\n"
            "║                                                                ║\n"
            "║  Agents require Node.js to run. To fix:                        ║\n"
            "║  1. Install Node.js 20+                                        ║\n"
            "║  2. Ensure 'node' is in PATH                                   ║\n"
            "║  3. Restart NanoClaw                                           ║\n"
            "╚════════════════════════════════════════════════════════════════╝\n\n");
        throw std::runtime_error("Node.js is required but not found in PATH");
    }

    // Check that agent-runner dist/index.js exists
    if (!fs::exists(cfg.agent_runner_path)) {
        logger()->error("Agent runner not found at {}", cfg.agent_runner_path);
        fprintf(stderr, "\n"
            "╔════════════════════════════════════════════════════════════════╗\n"
            "║  FATAL: Agent runner not built                                 ║\n"
            "║                                                                ║\n"
            "║  The agent-runner TypeScript must be compiled first:            ║\n"
            "║  1. cd container/agent-runner                                   ║\n"
            "║  2. npm install && npm run build                                ║\n"
            "║  3. Restart NanoClaw                                           ║\n"
            "╚════════════════════════════════════════════════════════════════╝\n\n");
        throw std::runtime_error("Agent runner not found at " + cfg.agent_runner_path);
    }

    logger()->info("Agent runner ready: {}", cfg.agent_runner_path);
}

void cleanup_orphan_processes() {
    // In direct-host mode, orphan processes are cleaned up naturally
    // when the parent exits or via OS-level process management.
    // No Docker container cleanup needed.
    logger()->debug("Orphan process cleanup: no-op in direct mode");
}

} // namespace nanoclaw
