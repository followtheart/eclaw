#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <memory>

namespace nanoclaw {

/**
 * Get the global logger instance.
 * Initializes on first call with colored console output.
 */
inline std::shared_ptr<spdlog::logger>& logger() {
    static auto instance = [] {
        auto log = spdlog::stdout_color_mt("nanoclaw");
        // Set level from LOG_LEVEL env var
        const char* level_env = std::getenv("LOG_LEVEL");
        if (level_env) {
            std::string level(level_env);
            if (level == "trace") log->set_level(spdlog::level::trace);
            else if (level == "debug") log->set_level(spdlog::level::debug);
            else if (level == "info") log->set_level(spdlog::level::info);
            else if (level == "warn") log->set_level(spdlog::level::warn);
            else if (level == "error") log->set_level(spdlog::level::err);
            else if (level == "fatal") log->set_level(spdlog::level::critical);
        } else {
            log->set_level(spdlog::level::info);
        }
        log->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
        return log;
    }();
    return instance;
}

} // namespace nanoclaw
