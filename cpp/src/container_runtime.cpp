#include "container_runtime.h"
#include "logger.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

#ifdef __linux__
#include <sys/utsname.h>
#endif

namespace nanoclaw {

const std::string CONTAINER_RUNTIME_BIN = "docker";

std::vector<std::string> host_gateway_args() {
#ifdef __linux__
    return {"--add-host=host.docker.internal:host-gateway"};
#else
    return {};
#endif
}

std::vector<std::string> readonly_mount_args(const std::string& host_path, const std::string& container_path) {
    return {"-v", host_path + ":" + container_path + ":ro"};
}

std::string stop_container_cmd(const std::string& name) {
    return CONTAINER_RUNTIME_BIN + " stop -t 1 " + name;
}

static std::string exec_cmd(const std::string& cmd) {
    std::array<char, 4096> buffer;
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }
    pclose(pipe);
    return result;
}

void ensure_container_runtime_running() {
    std::string cmd = CONTAINER_RUNTIME_BIN + " info 2>/dev/null";
    int ret = std::system(cmd.c_str());
    if (ret != 0) {
        logger()->error("Failed to reach container runtime");
        fprintf(stderr, "\n"
            "╔════════════════════════════════════════════════════════════════╗\n"
            "║  FATAL: Container runtime failed to start                      ║\n"
            "║                                                                ║\n"
            "║  Agents cannot run without a container runtime. To fix:        ║\n"
            "║  1. Ensure Docker is installed and running                     ║\n"
            "║  2. Run: docker info                                           ║\n"
            "║  3. Restart NanoClaw                                           ║\n"
            "╚════════════════════════════════════════════════════════════════╝\n\n");
        throw std::runtime_error("Container runtime is required but failed to start");
    }
    logger()->debug("Container runtime already running");
}

void cleanup_orphans() {
    try {
        std::string cmd = CONTAINER_RUNTIME_BIN + " ps --filter name=nanoclaw- --format '{{.Names}}' 2>/dev/null";
        auto output = exec_cmd(cmd);

        std::vector<std::string> orphans;
        std::istringstream stream(output);
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty()) orphans.push_back(line);
        }

        for (const auto& name : orphans) {
            std::system(stop_container_cmd(name).c_str());
        }

        if (!orphans.empty()) {
            logger()->info("Stopped {} orphaned containers", orphans.size());
        }
    } catch (const std::exception& e) {
        logger()->warn("Failed to clean up orphaned containers: {}", e.what());
    }
}

} // namespace nanoclaw
