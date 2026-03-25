#pragma once

#include <map>
#include <string>
#include <vector>

namespace nanoclaw {

/**
 * Parse the .env file and return values for the requested keys.
 * Does NOT load anything into the environment.
 */
std::map<std::string, std::string> read_env_file(const std::vector<std::string>& keys);

} // namespace nanoclaw
