#pragma once

#include <string>

namespace nanoclaw {

/**
 * Check whether a timezone string is a valid IANA identifier.
 * Uses the system's timezone database.
 */
bool is_valid_timezone(const std::string& tz);

/**
 * Return the given timezone if valid, otherwise fall back to UTC.
 */
std::string resolve_timezone(const std::string& tz);

/**
 * Convert a UTC ISO timestamp to a localized display string.
 */
std::string format_local_time(const std::string& utc_iso, const std::string& timezone);

/**
 * Get current time as ISO 8601 string.
 */
std::string now_iso();

} // namespace nanoclaw
