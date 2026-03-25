#include "router.h"
#include "timezone.h"

#include <regex>
#include <sstream>

namespace nanoclaw {

std::string escape_xml(const std::string& s) {
    if (s.empty()) return "";
    std::string result;
    result.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': result += "&amp;"; break;
            case '<': result += "&lt;"; break;
            case '>': result += "&gt;"; break;
            case '"': result += "&quot;"; break;
            default: result += c;
        }
    }
    return result;
}

std::string format_messages(const std::vector<NewMessage>& messages, const std::string& timezone) {
    std::ostringstream oss;
    oss << "<context timezone=\"" << escape_xml(timezone) << "\" />\n";
    oss << "<messages>\n";

    for (const auto& m : messages) {
        auto display_time = format_local_time(m.timestamp, timezone);
        oss << "<message sender=\"" << escape_xml(m.sender_name)
            << "\" time=\"" << escape_xml(display_time)
            << "\">" << escape_xml(m.content) << "</message>\n";
    }

    oss << "</messages>";
    return oss.str();
}

std::string strip_internal_tags(const std::string& text) {
    static const std::regex internal_re("<internal>[\\s\\S]*?</internal>");
    std::string result = std::regex_replace(text, internal_re, "");

    // Trim
    size_t start = result.find_first_not_of(" \t\n\r");
    size_t end = result.find_last_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    return result.substr(start, end - start + 1);
}

std::string format_outbound(const std::string& raw_text) {
    auto text = strip_internal_tags(raw_text);
    return text;
}

Channel* find_channel(const std::vector<std::shared_ptr<Channel>>& channels, const std::string& jid) {
    for (const auto& ch : channels) {
        if (ch->owns_jid(jid)) return ch.get();
    }
    return nullptr;
}

} // namespace nanoclaw
