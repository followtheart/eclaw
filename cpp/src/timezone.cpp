#include "timezone.h"
#include "platform.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <cstring>
#include <cstdlib>

namespace nanoclaw {

bool is_valid_timezone(const std::string& tz) {
    return platform::is_valid_timezone_name(tz);
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

    time_t utc_time = platform::timegm_portable(&tm_utc);

    // Convert to target timezone
    std::string resolved_tz = resolve_timezone(timezone);
    const char* old_tz = getenv("TZ");
    std::string old_tz_str = old_tz ? old_tz : "";

    platform::setenv_portable("TZ", resolved_tz);
#ifdef _WIN32
    _tzset();
#else
    tzset();
#endif

    struct tm tm_local;
    platform::localtime_safe(&utc_time, &tm_local);

    // Format: "Mar 14, 2025, 9:30 AM"
    char buf[128];
    strftime(buf, sizeof(buf), "%b %d, %Y, %I:%M %p", &tm_local);

    // Restore TZ
    if (!old_tz_str.empty()) {
        platform::setenv_portable("TZ", old_tz_str);
    } else {
        platform::unsetenv_portable("TZ");
    }
#ifdef _WIN32
    _tzset();
#else
    tzset();
#endif

    return std::string(buf);
}

std::string now_iso() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    struct tm tm_utc;
    platform::gmtime_safe(&time_t_now, &tm_utc);

    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_utc);

    std::ostringstream oss;
    oss << buf << "." << std::setfill('0') << std::setw(3) << ms.count() << "Z";
    return oss.str();
}

} // namespace nanoclaw
