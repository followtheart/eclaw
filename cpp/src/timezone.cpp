#include "timezone.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <cstring>
#include <cstdlib>

namespace nanoclaw {

bool is_valid_timezone(const std::string& tz) {
    if (tz.empty()) return false;
    // Try setting TZ and see if mktime handles it
    // A simple heuristic: check if the timezone file exists
    std::string tz_file = "/usr/share/zoneinfo/" + tz;
    FILE* f = fopen(tz_file.c_str(), "r");
    if (f) {
        fclose(f);
        return true;
    }
    // Also accept "UTC"
    if (tz == "UTC" || tz == "GMT") return true;
    return false;
}

std::string resolve_timezone(const std::string& tz) {
    return is_valid_timezone(tz) ? tz : "UTC";
}

std::string format_local_time(const std::string& utc_iso, const std::string& timezone) {
    // Parse ISO 8601 timestamp
    struct tm tm_utc = {};
    std::istringstream ss(utc_iso);
    ss >> std::get_time(&tm_utc, "%Y-%m-%dT%H:%M:%S");
    if (ss.fail()) {
        return utc_iso; // fallback
    }

    time_t utc_time = timegm(&tm_utc);

    // Convert to target timezone
    std::string resolved_tz = resolve_timezone(timezone);
    const char* old_tz = getenv("TZ");
    std::string old_tz_str = old_tz ? old_tz : "";

    setenv("TZ", resolved_tz.c_str(), 1);
    tzset();

    struct tm tm_local;
    localtime_r(&utc_time, &tm_local);

    // Format: "Mar 14, 2025, 9:30 AM"
    char buf[128];
    strftime(buf, sizeof(buf), "%b %d, %Y, %I:%M %p", &tm_local);

    // Restore TZ
    if (!old_tz_str.empty()) {
        setenv("TZ", old_tz_str.c_str(), 1);
    } else {
        unsetenv("TZ");
    }
    tzset();

    return std::string(buf);
}

std::string now_iso() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    struct tm tm_utc;
    gmtime_r(&time_t_now, &tm_utc);

    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_utc);

    std::ostringstream oss;
    oss << buf << "." << std::setfill('0') << std::setw(3) << ms.count() << "Z";
    return oss.str();
}

} // namespace nanoclaw
