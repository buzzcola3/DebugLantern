#ifndef DEBUGLANTERN_COMMON_H
#define DEBUGLANTERN_COMMON_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace debuglantern {

bool set_nonblocking(int fd);

std::string json_escape(const std::string &input);
std::string json_kv(const std::string &key, const std::string &value, bool quote);
std::string json_kv(const std::string &key, long long value);
std::string json_kv(const std::string &key, bool value);

std::string now_iso8601();

}  // namespace debuglantern

#endif  // DEBUGLANTERN_COMMON_H
