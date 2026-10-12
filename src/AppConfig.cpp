#include "AppConfig.h"

#include "LocalSendProtocol.h"

#include <fstream>

namespace RomCloud {

AppConfig &AppConfig::instance() {
  static AppConfig config;
  return config;
}

void AppConfig::setDataDirectory(const std::string &path) {
  m_dataDirectory = path;
  LsUtil::mkdirRecursive(m_dataDirectory);
}

std::string AppConfig::readValue(const std::string &name) const {
  std::ifstream stream(m_dataDirectory + "/" + name);
  std::string value;
  std::getline(stream, value);
  while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) {
    value.pop_back();
  }
  return value;
}

void AppConfig::writeValue(const std::string &name,
                           const std::string &value) const {
  LsUtil::mkdirRecursive(m_dataDirectory);
  std::ofstream stream(m_dataDirectory + "/" + name,
                       std::ios::trunc | std::ios::binary);
  stream << value << '\n';
}

std::string AppConfig::getLocalSendAlias() const {
  std::string alias = readValue("alias");
  if (!alias.empty()) {
    return alias;
  }
  const std::string generated = "Trimui-" + LsUtil::randomHex(4);
  const_cast<AppConfig *>(this)->setLocalSendAlias(generated);
  return generated;
}

void AppConfig::setLocalSendAlias(const std::string &alias) {
  writeValue("alias", alias);
}

std::string AppConfig::getOrCreateLocalSendFingerprint() {
  std::string fingerprint = readValue("fingerprint");
  if (!fingerprint.empty()) {
    return fingerprint;
  }
  fingerprint = LsUtil::randomHex(64);
  writeValue("fingerprint", fingerprint);
  return fingerprint;
}

} // namespace RomCloud
