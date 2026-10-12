#pragma once

#include <string>

namespace RomCloud {

class AppConfig {
public:
  static AppConfig &instance();
  void setDataDirectory(const std::string &path);
  std::string getLocalSendAlias() const;
  void setLocalSendAlias(const std::string &alias);
  std::string getOrCreateLocalSendFingerprint();

private:
  AppConfig() = default;
  std::string readValue(const std::string &name) const;
  void writeValue(const std::string &name, const std::string &value) const;
  std::string m_dataDirectory = "/mnt/SDCARD/Apps/Trimui-LocalSend/data";
};

} // namespace RomCloud
