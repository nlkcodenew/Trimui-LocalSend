#include "AppConfig.h"

#include "LocalSendProtocol.h"

#include <fstream>
#include <sys/stat.h>
#include <vector>

#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

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

std::string AppConfig::deviceCertPath() const {
  return m_dataDirectory + "/device.crt";
}

std::string AppConfig::deviceKeyPath() const {
  return m_dataDirectory + "/device.key";
}

static bool addCertExt(X509 *cert, int nid, const char *value) {
  X509V3_CTX ctx;
  X509V3_set_ctx_nodb(&ctx);
  X509V3_set_ctx(&ctx, cert, cert, nullptr, nullptr, 0);
  X509_EXTENSION *ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value);
  if (!ext)
    return false;
  bool ok = X509_add_ext(cert, ext, -1) == 1;
  X509_EXTENSION_free(ext);
  return ok;
}

bool AppConfig::ensureDeviceIdentity() {
  const std::string certPath = deviceCertPath();
  const std::string keyPath = deviceKeyPath();
  struct stat st;
  if (stat(certPath.c_str(), &st) == 0 && stat(keyPath.c_str(), &st) == 0)
    return true;
  LsUtil::mkdirRecursive(m_dataDirectory);

  // Sinh khoa RSA 2048 bang keygen-context API (chay ca OpenSSL 1.1/3.x)
  EVP_PKEY_CTX *kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
  EVP_PKEY *pkey = nullptr;
  bool ok = kctx && EVP_PKEY_keygen_init(kctx) > 0 &&
            EVP_PKEY_CTX_set_rsa_keygen_bits(kctx, 2048) > 0 &&
            EVP_PKEY_keygen(kctx, &pkey) > 0 && pkey != nullptr;
  if (kctx)
    EVP_PKEY_CTX_free(kctx);
  if (!ok) {
    if (pkey)
      EVP_PKEY_free(pkey);
    return false;
  }

  X509 *cert = X509_new();
  if (!cert) {
    EVP_PKEY_free(pkey);
    return false;
  }
  X509_set_version(cert, 2);
  unsigned char serialBytes[8];
  RAND_bytes(serialBytes, sizeof(serialBytes));
  serialBytes[0] = (unsigned char)((serialBytes[0] & 0x7F) | 0x01);
  BIGNUM *bn = BN_bin2bn(serialBytes, sizeof(serialBytes), nullptr);
  ASN1_INTEGER *serial = BN_to_ASN1_INTEGER(bn, nullptr);
  if (serial) {
    X509_set_serialNumber(cert, serial);
    ASN1_INTEGER_free(serial);
  }
  if (bn)
    BN_free(bn);

  const std::string cn = getLocalSendAlias();
  X509_NAME *name = X509_get_subject_name(cert);
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                             (const unsigned char *)cn.c_str(), -1, -1, 0);
  X509_set_issuer_name(cert, name);
  X509_gmtime_adj(X509_getm_notBefore(cert), -86400L);
  X509_gmtime_adj(X509_getm_notAfter(cert), 60L * 60 * 24 * 3650);
  X509_set_pubkey(cert, pkey);
  addCertExt(cert, NID_basic_constraints, "critical,CA:FALSE");
  addCertExt(cert, NID_key_usage, "critical,digitalSignature,keyEncipherment");
  addCertExt(cert, NID_ext_key_usage, "serverAuth,clientAuth");
  ok = X509_sign(cert, pkey, EVP_sha256()) > 0;

  if (ok) {
    FILE *kf = fopen(keyPath.c_str(), "w");
    if (!kf) {
      ok = false;
    } else {
      ok = PEM_write_PrivateKey(kf, pkey, nullptr, nullptr, 0, nullptr,
                                nullptr) == 1;
      fclose(kf);
      chmod(keyPath.c_str(), 0600);
    }
  }
  if (ok) {
    FILE *cf = fopen(certPath.c_str(), "w");
    if (!cf) {
      ok = false;
    } else {
      ok = PEM_write_X509(cf, cert) == 1;
      fclose(cf);
    }
  }
  X509_free(cert);
  EVP_PKEY_free(pkey);
  return ok;
}

static std::string fingerprintOfCertFile(const std::string &path) {
  FILE *fp = fopen(path.c_str(), "r");
  if (!fp)
    return "";
  X509 *cert = PEM_read_X509(fp, nullptr, nullptr, nullptr);
  fclose(fp);
  if (!cert)
    return "";
  std::string out;
  int len = i2d_X509(cert, nullptr);
  if (len > 0) {
    std::vector<unsigned char> der((size_t)len);
    unsigned char *p = der.data();
    i2d_X509(cert, &p);
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(der.data(), der.size(), digest);
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
      out += hexd[digest[i] >> 4];
      out += hexd[digest[i] & 0x0F];
    }
  }
  X509_free(cert);
  return out;
}

std::string AppConfig::getOrCreateLocalSendFingerprint() {
  // Chuan LocalSend: fingerprint = SHA-256 cua cert thiet bi. Ban cu
  // dung chuoi hex ngau nhien — receiver chinh hang doi chieu cert
  // client (mTLS) voi fingerprint nay nen luon tu choi. Tu dong
  // chuyen doi: co cert la tinh lai fingerprint va ghi de file cu.
  if (ensureDeviceIdentity()) {
    std::string derived = fingerprintOfCertFile(deviceCertPath());
    if (!derived.empty()) {
      if (readValue("fingerprint") != derived)
        writeValue("fingerprint", derived);
      return derived;
    }
  }
  // Du phong (sinh cert that bai): giu hanh vi cu de app van chay.
  std::string fingerprint = readValue("fingerprint");
  if (!fingerprint.empty()) {
    return fingerprint;
  }
  fingerprint = LsUtil::randomHex(64);
  writeValue("fingerprint", fingerprint);
  return fingerprint;
}

} // namespace RomCloud
