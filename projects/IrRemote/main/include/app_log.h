#pragma once

#include <string>

namespace applog {

void begin_session(const std::string &title);
void debug(const std::string &message);
void info(const std::string &message);
void warn(const std::string &message);
void error(const std::string &message);
std::string log_path();

}  // namespace applog
