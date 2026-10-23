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
  // Danh tinh TLS cua thiet bi cho giao thuc LocalSend: cap cert/key
  // tu ky luu ben trong data/. Fingerprint chuan cua giao thuc la
  // SHA-256 (hex) cua cert dang DER — receiver chinh hang doi chieu
  // fingerprint cong bo voi cert client trinh trong bat tay mTLS.
  bool ensureDeviceIdentity();
  std::string deviceCertPath() const;
  std::string deviceKeyPath() const;

private:
  AppConfig() = default;
  std::string readValue(const std::string &name) const;
  void writeValue(const std::string &name, const std::string &value) const;
  std::string m_dataDirectory = "/mnt/SDCARD/Apps/Trimui-LocalSend/data";
};

} // namespace RomCloud
