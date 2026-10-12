// OtaVerify.cpp — verify manifest OTA ký Ed25519 bằng OpenSSL EVP.
// Mô hình học từ LottoForecast-Pro (client/src/ota.rs):
//   - manifest JSON: {version, kid, files:[{path, sha256, size}], signature}
//   - signature = Ed25519(canonical_body), base64url không padding
//   - canonical_body = {"files":[...],"kid":"...","version":"..."} (key sắp xếp,
//     files sắp xếp theo path), serialize gọn không khoảng trắng
// Private key ký release giữ offline trên máy dev; binary chỉ nhúng public key.
#include "OtaVerify.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

#include <openssl/evp.h>

namespace {

const OtaReleaseKey kKeys[] = {
    {"ls1", "ZQfawnqGbs5nChUhWLEWxA5LNZki5g-0JT-rc3ZKUaE"},
};

// ---------- base64url decode (không padding) ----------

int b64Val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '-') return 62;
  if (c == '_') return 63;
  return -1;
}

bool b64UrlDecode(const std::string &in, std::vector<uint8_t> &out) {
  out.clear();
  int bits = 0;
  uint32_t acc = 0;
  for (char c : in) {
    if (c == '=') break;
    int v = b64Val(c);
    if (v < 0) return false;
    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back((uint8_t)(acc >> bits));
      acc &= (bits == 0) ? 0 : ((1u << bits) - 1);
    }
  }
  return true;
}

// ---------- JSON parser tối thiểu (đủ cho manifest) ----------

struct JVal {
  enum Type { NUL, STR, NUM, ARR, OBJ } type = NUL;
  std::string s;
  double num = 0;
  std::vector<JVal> arr;
  std::map<std::string, JVal> obj;
};

struct JParser {
  const char *p;
  const char *end;
  bool fail = false;

  explicit JParser(const std::string &text)
      : p(text.data()), end(text.data() + text.size()) {}

  void skipWs() {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
  }
  bool expect(char c) {
    skipWs();
    if (p < end && *p == c) {
      ++p;
      return true;
    }
    fail = true;
    return false;
  }
  JVal parseValue() {
    skipWs();
    JVal v;
    if (p >= end) {
      fail = true;
      return v;
    }
    if (*p == '"') {
      v.type = JVal::STR;
      v.s = parseString();
    } else if (*p == '{') {
      v = parseObject();
    } else if (*p == '[') {
      v = parseArray();
    } else if ((*p >= '0' && *p <= '9') || *p == '-') {
      v.type = JVal::NUM;
      v.num = parseNumber();
    } else {
      fail = true;
    }
    return v;
  }
  std::string parseString() {
    std::string out;
    ++p;  // opening quote
    while (p < end && *p != '"') {
      if (*p == '\\' && p + 1 < end) {
        ++p;
        switch (*p) {
          case '"': out += '"'; break;
          case '\\': out += '\\'; break;
          case '/': out += '/'; break;
          case 'n': out += '\n'; break;
          case 't': out += '\t'; break;
          case 'r': out += '\r'; break;
          default: out += *p; break;
        }
        ++p;
      } else {
        out += *p++;
      }
    }
    if (p >= end) {
      fail = true;
      return out;
    }
    ++p;  // closing quote
    return out;
  }
  double parseNumber() {
    const char *start = p;
    while (p < end &&
           (isdigit((unsigned char)*p) || *p == '-' || *p == '+' || *p == '.' ||
            *p == 'e' || *p == 'E'))
      ++p;
    return strtod(std::string(start, p).c_str(), nullptr);
  }
  JVal parseObject() {
    JVal v;
    v.type = JVal::OBJ;
    ++p;  // {
    skipWs();
    if (p < end && *p == '}') {
      ++p;
      return v;
    }
    while (p < end) {
      skipWs();
      if (p >= end || *p != '"') {
        fail = true;
        return v;
      }
      std::string key = parseString();
      if (!expect(':')) return v;
      v.obj[key] = parseValue();
      skipWs();
      if (p < end && *p == ',') {
        ++p;
        continue;
      }
      break;
    }
    if (!expect('}')) return v;
    return v;
  }
  JVal parseArray() {
    JVal v;
    v.type = JVal::ARR;
    ++p;  // [
    skipWs();
    if (p < end && *p == ']') {
      ++p;
      return v;
    }
    while (p < end) {
      v.arr.push_back(parseValue());
      skipWs();
      if (p < end && *p == ',') {
        ++p;
        continue;
      }
      break;
    }
    if (!expect(']')) return v;
    return v;
  }
};

const JVal *jGet(const JVal &obj, const char *key) {
  if (obj.type != JVal::OBJ) return nullptr;
  auto it = obj.obj.find(key);
  return it == obj.obj.end() ? nullptr : &it->second;
}

// JSON-escape cho canonical body (đồng nhất với make_manifest.py).
std::string jEscape(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 2);
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '\r': out += "\\r"; break;
      default: out += c; break;
    }
  }
  return out;
}

bool safeRelPath(const std::string &path) {
  if (path.empty() || path.find('\\') != std::string::npos) return false;
  // Chỉ cho phép segment bình thường, cấm absolute và "..".
  size_t i = 0;
  while (i <= path.size()) {
    size_t j = path.find('/', i);
    std::string seg = (j == std::string::npos) ? path.substr(i)
                                               : path.substr(i, j - i);
    if (seg.empty() || seg == "." || seg == "..") return false;
    if (j == std::string::npos) break;
    i = j + 1;
  }
  return true;
}

bool isHex64(const std::string &s) {
  if (s.size() != 64) return false;
  for (char c : s)
    if (!isxdigit((unsigned char)c)) return false;
  return true;
}

bool ed25519Verify(const std::vector<uint8_t> &pub,
                   const std::string &msg,
                   const std::vector<uint8_t> &sig) {
  if (pub.size() != 32 || sig.size() != 64) return false;
  EVP_PKEY *pkey =
      EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, pub.data(), pub.size());
  if (!pkey) return false;
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  bool ok = false;
  if (ctx &&
      EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, pkey) == 1 &&
      EVP_DigestVerify(ctx, sig.data(), sig.size(),
                       reinterpret_cast<const uint8_t *>(msg.data()),
                       msg.size()) == 1) {
    ok = true;
  }
  EVP_MD_CTX_free(ctx);
  EVP_PKEY_free(pkey);
  return ok;
}

}  // namespace

const OtaReleaseKey *OtaReleaseKeys() { return kKeys; }
int OtaReleaseKeyCount() {
  return (int)(sizeof(kKeys) / sizeof(kKeys[0]));
}

bool OtaVerifyManifest(const std::string &manifestPath) {
  std::ifstream in(manifestPath, std::ios::binary);
  if (!in) {
    fprintf(stderr, "[ota] khong mo duoc manifest: %s\n", manifestPath.c_str());
    return false;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  std::string text = ss.str();  // Giữ string sống trong lúc parse (tránh dangling)
  JParser parser(text);
  JVal root = parser.parseValue();
  if (parser.fail || root.type != JVal::OBJ) {
    fprintf(stderr, "[ota] manifest khong phai JSON hop le\n");
    return false;
  }
  const JVal *jVer = jGet(root, "version");
  const JVal *jKid = jGet(root, "kid");
  const JVal *jSig = jGet(root, "signature");
  const JVal *jFiles = jGet(root, "files");
  if (!jVer || jVer->type != JVal::STR || jVer->s.empty() ||
      !jKid || jKid->type != JVal::STR || jKid->s.empty() ||
      !jSig || jSig->type != JVal::STR || jSig->s.empty() ||
      !jFiles || jFiles->type != JVal::ARR || jFiles->arr.empty()) {
    fprintf(stderr, "[ota] manifest thieu truong bat buoc\n");
    return false;
  }

  struct FileEntry {
    std::string path;
    std::string sha256;
    uint64_t size;
  };
  std::vector<FileEntry> files;
  for (const JVal &f : jFiles->arr) {
    const JVal *jp = jGet(f, "path");
    const JVal *js = jGet(f, "sha256");
    const JVal *jz = jGet(f, "size");
    if (!jp || jp->type != JVal::STR || !js || js->type != JVal::STR ||
        !jz || jz->type != JVal::NUM) {
      fprintf(stderr, "[ota] file entry khong hop le\n");
      return false;
    }
    if (!safeRelPath(jp->s) || !isHex64(js->s) || jz->num <= 0 ||
        jz->num > (double)(512ULL * 1024 * 1024)) {
      fprintf(stderr, "[ota] file entry khong hop le: %s\n", jp->s.c_str());
      return false;
    }
    files.push_back({jp->s, js->s, (uint64_t)jz->num});
  }
  std::sort(files.begin(), files.end(),
            [](const FileEntry &a, const FileEntry &b) { return a.path < b.path; });
  for (size_t i = 1; i < files.size(); ++i) {
    if (files[i].path == files[i - 1].path) {
      fprintf(stderr, "[ota] path trung lap: %s\n", files[i].path.c_str());
      return false;
    }
  }

  // Canonical body — phải khớp từng byte với make_manifest.py.
  std::string body = "{\"files\":[";
  for (size_t i = 0; i < files.size(); ++i) {
    if (i) body += ",";
    body += "{\"path\":\"" + jEscape(files[i].path) + "\",\"sha256\":\"" +
            files[i].sha256 + "\",\"size\":" + std::to_string(files[i].size) + "}";
  }
  body += "],\"kid\":\"" + jEscape(jKid->s) + "\",\"version\":\"" +
          jEscape(jVer->s) + "\"}";

  std::vector<uint8_t> sig;
  if (!b64UrlDecode(jSig->s, sig) || sig.size() != 64) {
    fprintf(stderr, "[ota] signature khong hop le\n");
    return false;
  }
  const OtaReleaseKey *key = nullptr;
  for (int i = 0; i < OtaReleaseKeyCount(); ++i) {
    if (jKid->s == OtaReleaseKeys()[i].kid) {
      key = &OtaReleaseKeys()[i];
      break;
    }
  }
  if (!key) {
    fprintf(stderr, "[ota] kid khong co public key: %s\n", jKid->s.c_str());
    return false;
  }
  std::vector<uint8_t> pub;
  if (!b64UrlDecode(key->pubB64Url, pub) || pub.size() != 32) {
    fprintf(stderr, "[ota] public key loi\n");
    return false;
  }
  if (!ed25519Verify(pub, body, sig)) {
    fprintf(stderr, "[ota] chu ky Ed25519 KHONG hop le\n");
    return false;
  }
  fprintf(stderr, "[ota] manifest hop le: version=%s kid=%s (%zu files)\n",
          jVer->s.c_str(), jKid->s.c_str(), files.size());
  return true;
}
