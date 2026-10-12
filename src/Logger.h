#pragma once

#include <string>

namespace RomCloud {

class Logger {
public:
  static void setLogPath(const std::string &path);
  static void info(const std::string &message);
  static void warn(const std::string &message);
  static void error(const std::string &message);
};

} // namespace RomCloud
