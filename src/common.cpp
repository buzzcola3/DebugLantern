#include "common.h"

#include <chrono>
#include <ctime>
#include <fcntl.h>
#include <sstream>
#include <string>
#include <sys/types.h>
#include <unistd.h>

namespace debuglantern {

bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

std::string json_escape(const std::string &input) {
    std::string out;
    out.reserve(input.size());
    for (char c : input) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += "?";
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

std::string json_kv(const std::string &key, const std::string &value, bool quote) {
    std::ostringstream oss;
    oss << "\"" << json_escape(key) << "\":";
    if (quote) {
        oss << "\"" << json_escape(value) << "\"";
    } else {
        oss << value;
    }
    return oss.str();
}

std::string json_kv(const std::string &key, long long value) {
    std::ostringstream oss;
    oss << "\"" << json_escape(key) << "\":" << value;
    return oss.str();
}

std::string json_kv(const std::string &key, bool value) {
    std::ostringstream oss;
    oss << "\"" << json_escape(key) << "\":" << (value ? "true" : "false");
    return oss.str();
}

std::string now_iso8601() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}

}  // namespace debuglantern
