#include "Logger.h"

#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>

namespace RomCloud {
namespace {
std::mutex logMutex;
std::string logPath; // rỗng = chỉ ghi stderr (khi chưa gọi setLogPath)

void writeLog(const char *level, const std::string &message) {
  std::lock_guard<std::mutex> lock(logMutex);
  const std::time_t now = std::time(nullptr);
  char timestamp[32] = {};
  std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S",
                std::localtime(&now));
  if (!logPath.empty()) {
    // Chỉ ghi file, không ghi thêm stderr: launch.sh redirect stderr (2>&1)
    // vào cùng file log nên bản cũ mỗi dòng bị nhân đôi.
    std::ofstream stream(logPath, std::ios::app);
    if (stream) {
      stream << timestamp << " [" << level << "] " << message << '\n';
    }
    return;
  }
  std::fprintf(stderr, "%s [%s] %s\n", timestamp, level, message.c_str());
}
} // namespace

void Logger::setLogPath(const std::string &path) { logPath = path; }
void Logger::info(const std::string &message) { writeLog("INFO", message); }
void Logger::warn(const std::string &message) { writeLog("WARN", message); }
void Logger::error(const std::string &message) { writeLog("ERROR", message); }

} // namespace RomCloud
