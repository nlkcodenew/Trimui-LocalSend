// SaveSync.cpp
// Quét và đồng bộ save game giữa 2 máy TrimUI, kể cả khác OS.
// Layout vật lý khác nhau (stock vs Knulli) được dịch qua canonical path.

#include "SaveSync.h"
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>
#include <algorithm>

namespace RomCloud {

namespace {

bool startsWith(const std::string& s, const std::string& p) {
  return s.compare(0, p.size(), p) == 0;
}
bool endsWith(const std::string& s, const std::string& suffix) {
  if (suffix.size() > s.size()) return false;
  return s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}
std::string trimSlashes(std::string s) {
  while (!s.empty() && s.front() == '/') s.erase(s.begin());
  while (!s.empty() && s.back() == '/') s.pop_back();
  return s;
}
std::string joinPath(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  return (a.back() == '/') ? (a + b) : (a + "/" + b);
}
bool pathExists(const std::string& p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0;
}
bool dirExists(const std::string& p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
void makeDirs(const std::string& p) {
  if (p.empty() || dirExists(p)) return;
  size_t pos = p.rfind('/');
  if (pos != std::string::npos && pos > 0) makeDirs(p.substr(0, pos));
  mkdir(p.c_str(), 0755);
}
// Danh sách thư mục con (bỏ . và ..) sắp xếp alpha
std::vector<std::string> listSubdirs(const std::string& dir) {
  std::vector<std::string> out;
  DIR* d = opendir(dir.c_str());
  if (!d) return out;
  struct dirent* e;
  while ((e = readdir(d)) != nullptr) {
    if (e->d_name[0] == '.') continue;
    std::string full = joinPath(dir, e->d_name);
    struct stat st;
    if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
      out.push_back(e->d_name);
  }
  closedir(d);
  std::sort(out.begin(), out.end());
  return out;
}
// Quét đệ quy, gọi cb cho mỗi file thường: cb(canonicalRelDir, fileName, absPath, size, mtime)
template <typename Cb>
void scanDirRecursive(const std::string& baseDir, const std::string& relDir,
                      const std::string& canonDir, Cb&& cb) {
  std::string fullPath = joinPath(baseDir, relDir);
  DIR* dir = opendir(fullPath.c_str());
  if (!dir) return;
  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    std::string childRel = relDir.empty() ? entry->d_name : relDir + "/" + entry->d_name;
    std::string childCanon = canonDir.empty() ? entry->d_name : canonDir + "/" + entry->d_name;
    std::string childFull = joinPath(baseDir, childRel);
    struct stat st;
    if (stat(childFull.c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      scanDirRecursive(baseDir, childRel, childCanon, cb);
    } else if (S_ISREG(st.st_mode)) {
      cb(relDir.empty() ? canonDir : childCanon.substr(0, childCanon.rfind('/')),
         entry->d_name, childFull,
         static_cast<uint64_t>(st.st_size), static_cast<int64_t>(st.st_mtime));
    }
  }
  closedir(dir);
}
// Quét đệ quy, chỉ file có đuôi cho trước (vd ".srm", so không phân biệt hoa thường)
template <typename Cb>
void scanDirRecursiveExt(const std::string& baseDir, const std::string& relDir,
                         const std::string& canonDir, const std::string& ext,
                         const std::string& system, Cb&& cb) {
  std::string fullPath = joinPath(baseDir, relDir);
  DIR* dir = opendir(fullPath.c_str());
  if (!dir) return;
  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    std::string childRel = relDir.empty() ? entry->d_name : relDir + "/" + entry->d_name;
    std::string childCanon = canonDir.empty() ? entry->d_name : canonDir + "/" + entry->d_name;
    std::string childFull = joinPath(baseDir, childRel);
    struct stat st;
    if (stat(childFull.c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      scanDirRecursiveExt(baseDir, childRel, childCanon, ext, system, cb);
    } else if (S_ISREG(st.st_mode)) {
      std::string name = entry->d_name;
      std::string lower = name;
      std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
      if (!endsWith(lower, ext)) continue;
      cb(relDir.empty() ? canonDir : childCanon.substr(0, childCanon.rfind('/')),
         name, childFull, system,
         static_cast<uint64_t>(st.st_size), static_cast<int64_t>(st.st_mtime));
    }
  }
  closedir(dir);
}

} // namespace

SaveLayout detectSaveLayout() {
  static SaveLayout cached = SaveLayout::Stock;
  static bool inited = false;
  if (!inited) {
    inited = true;
    const char* env = getenv("SAVE_LAYOUT");
    if (env) {
      if (strcmp(env, "knulli") == 0) { cached = SaveLayout::Knulli; return cached; }
      if (strcmp(env, "stock") == 0) { cached = SaveLayout::Stock; return cached; }
    }
    struct stat st;
    if (stat("/userdata/system/knulli.conf", &st) == 0)
      cached = SaveLayout::Knulli;
  }
  return cached;
}

const char* saveLayoutName(SaveLayout l) {
  return l == SaveLayout::Knulli ? "knulli" : "stock";
}

std::string normalizeSaveRel(const std::string& rel) {
  std::string s = trimSlashes(rel);
  if (s == "ra" || startsWith(s, "ra/")) return s;
  if (s == "psp" || startsWith(s, "psp/")) return s;
  // Legacy: physical rel của binary cũ (stock) -> canonical
  const char* pspMarker = "/.config/ppsspp/PSP/";
  size_t pos = s.find(pspMarker);
  std::string tail;
  if (pos != std::string::npos) {
    tail = s.substr(pos + strlen(pspMarker));
  } else if (startsWith(s, ".config/ppsspp/PSP/")) {
    tail = s.substr(strlen(".config/ppsspp/PSP/"));
  }
  if (!tail.empty()) {
    if (startsWith(tail, "SAVEDATA")) return "psp/savedata" + tail.substr(8);
    if (startsWith(tail, "PPSSPP_STATE")) return "psp/state" + tail.substr(12);
    return s;
  }
  const char* raPrefix = "RetroArch/.retroarch/saves";
  if (s == raPrefix) return "ra";
  if (startsWith(s, std::string(raPrefix) + "/"))
    return "ra" + s.substr(strlen(raPrefix));
  return s;
}

// ---- Scan: stock layout ----
static void scanStock(const std::string& root, std::vector<SaveFileInfo>& out) {
  auto emit = [&](const std::string& canonDir, const std::string& name,
                  const std::string& abs, uint64_t size, int64_t mtime) {
    SaveFileInfo info;
    info.relPath = canonDir;
    info.fileName = name;
    info.size = size;
    info.mtime = mtime;
    info.absPath = abs;
    out.push_back(info);
  };
  // PPSSPP chính: .config/ppsspp/PSP/{SAVEDATA,PPSSPP_STATE}
  scanDirRecursive(root, ".config/ppsspp/PSP/SAVEDATA", "psp/savedata", emit);
  scanDirRecursive(root, ".config/ppsspp/PSP/PPSSPP_STATE", "psp/state", emit);
  // RetroArch battery saves (phẳng)
  scanDirRecursive(root, "RetroArch/.retroarch/saves", "ra", emit);
  // PPSSPP portable: Emus/PPSSPP/<version>/.config/ppsspp/PSP/{...}
  // -> gộp vào cùng namespace canonical (ghi: ưu tiên nơi đã có file)
  std::string emuDir = joinPath(root, "Emus/PPSSPP");
  for (const auto& ver : listSubdirs(emuDir)) {
    std::string base = "Emus/PPSSPP/" + ver + "/.config/ppsspp/PSP";
    if (!dirExists(joinPath(root, base))) continue;
    scanDirRecursive(root, base + "/SAVEDATA", "psp/savedata", emit);
    scanDirRecursive(root, base + "/PPSSPP_STATE", "psp/state", emit);
  }
}

// ---- Scan: Knulli layout (/userdata) ----
static void scanKnulli(const std::string& root, std::vector<SaveFileInfo>& out) {
  std::string saves = joinPath(root, "saves");
  // RetroArch: saves/<system>/*.srm (bỏ qua psp — xử lý riêng dưới)
  for (const auto& sys : listSubdirs(saves)) {
    if (sys == "psp") continue;
    auto emit = [&](const std::string& canonDir, const std::string& name,
                    const std::string& abs, const std::string& system,
                    uint64_t size, int64_t mtime) {
      SaveFileInfo info;
      info.relPath = canonDir;
      info.fileName = name;
      info.size = size;
      info.mtime = mtime;
      info.absPath = abs;
      info.system = system;
      out.push_back(info);
    };
    scanDirRecursiveExt(root, "saves/" + sys, "ra", ".srm", sys, emit);
  }
  // PSP (PPSSPP): saves/psp/PSP/{SAVEDATA,PPSSPP_STATE} (+ SAVEDATA phụ)
  auto emitPsp = [&](const std::string& canonDir, const std::string& name,
                     const std::string& abs, uint64_t size, int64_t mtime) {
    SaveFileInfo info;
    info.relPath = canonDir;
    info.fileName = name;
    info.size = size;
    info.mtime = mtime;
    info.absPath = abs;
    out.push_back(info);
  };
  scanDirRecursive(root, "saves/psp/PSP/SAVEDATA", "psp/savedata", emitPsp);
  scanDirRecursive(root, "saves/psp/PSP/PPSSPP_STATE", "psp/state", emitPsp);
  scanDirRecursive(root, "saves/psp/SAVEDATA", "psp/savedata", emitPsp);
}

std::vector<SaveFileInfo> SaveSync::scanLocalSaves(const std::string& root) {
  std::vector<SaveFileInfo> out;
  if (detectSaveLayout() == SaveLayout::Knulli)
    scanKnulli(root, out);
  else
    scanStock(root, out);
  return out;
}

std::vector<SaveFileInfo> SaveSync::diffForDownload(
    const std::vector<SaveFileInfo>& localList,
    const std::vector<SaveFileInfo>& remoteList) {
  std::vector<SaveFileInfo> needDownload;
  for (const auto& remote : remoteList) {
    std::string key = remote.relPath + "/" + remote.fileName;
    const SaveFileInfo* localMatch = nullptr;
    for (const auto& local : localList) {
      std::string lkey = local.relPath + "/" + local.fileName;
      if (lkey == key) { localMatch = &local; break; }
    }
    if (!localMatch) {
      needDownload.push_back(remote);
    } else if (remote.mtime > localMatch->mtime + 2) {
      needDownload.push_back(remote);
    } else if (remote.size != localMatch->size &&
               remote.mtime >= localMatch->mtime) {
      needDownload.push_back(remote);
    }
  }
  return needDownload;
}

// JSON: [{"rel":"...","name":"...","size":123,"mtime":456,"sys":"..."},...]
// "sys" optional — binary cũ bỏ qua khi parse.
std::string SaveSync::toJson(const std::vector<SaveFileInfo>& list) {
  std::string json = "[";
  for (size_t i = 0; i < list.size(); ++i) {
    if (i > 0) json += ",";
    const auto& f = list[i];
    json += "{\"rel\":\"" + f.relPath + "\",\"name\":\"" + f.fileName + "\",";
    json += "\"size\":" + std::to_string(f.size) + ",";
    json += "\"mtime\":" + std::to_string(f.mtime) + ",";
    json += "\"sys\":\"" + f.system + "\"}";
  }
  json += "]";
  return json;
}

std::vector<SaveFileInfo> SaveSync::fromJson(const std::string& json) {
  std::vector<SaveFileInfo> out;
  size_t pos = 0;
  while ((pos = json.find("{\"rel\":\"", pos)) != std::string::npos) {
    SaveFileInfo info;
    pos += 8; // len("{\"rel\":\"")
    size_t end = json.find("\"", pos);
    info.relPath = normalizeSaveRel(json.substr(pos, end - pos));
    pos = json.find("\"name\":\"", end) + 8;
    end = json.find("\"", pos);
    info.fileName = json.substr(pos, end - pos);
    pos = json.find("\"size\":", end) + 7;
    end = json.find(",", pos);
    info.size = std::stoull(json.substr(pos, end - pos));
    pos = json.find("\"mtime\":", end) + 8;
    end = json.find(",", pos);
    if (end == std::string::npos) end = json.find("}", pos);
    info.mtime = std::stoll(json.substr(pos, end - pos));
    // "sys" optional (binary cũ không gửi)
    size_t spos = json.find("\"sys\":\"", end);
    size_t epos = json.find("}", end);
    if (spos != std::string::npos && (epos == std::string::npos || spos < epos)) {
      spos += 7;
      size_t send = json.find("\"", spos);
      info.system = json.substr(spos, send - spos);
    }
    out.push_back(info);
    pos = end;
  }
  return out;
}

// ---- resolveLocalPath ----
namespace {
std::string firstSegment(const std::string& s) {
  size_t p = s.find('/');
  return p == std::string::npos ? s : s.substr(0, p);
}
// Chọn root PSP vật lý: ưu tiên nơi đã có <gameId>
std::string pickPspRoot(
                        const std::vector<std::string>& candidates,
                        const std::string& gameId) {
  for (const auto& c : candidates) {
    if (!gameId.empty() && dirExists(joinPath(c, gameId))) return c;
  }
  return candidates.empty() ? "" : candidates[0];
}
} // namespace

std::string SaveSync::resolveLocalPath(const std::string& root,
                                       const std::string& canonicalRel,
                                       const std::string& fileName,
                                       const std::string& systemHint,
                                       bool createDirs) {
  std::string rel = normalizeSaveRel(canonicalRel);
  bool isRa = (rel == "ra" || startsWith(rel, "ra/"));
  bool isPsp = (rel == "psp" || startsWith(rel, "psp/"));
  if ((!isRa && !isPsp) || fileName.empty() ||
      fileName == "." || fileName == ".." ||
      fileName.find('/') != std::string::npos)
    return "";
  std::string tail = (rel.size() > 4) ? rel.substr(4) : ""; // sau "psp/"
  if (isRa) tail = (rel.size() > 3) ? rel.substr(3) : "";    // sau "ra/"

  std::string result;
  if (detectSaveLayout() == SaveLayout::Knulli) {
    std::string saves = joinPath(root, "saves");
    if (isRa) {
      // 1) nơi đã có file này
      for (const auto& sys : listSubdirs(saves)) {
        if (sys == "psp") continue;
        std::string p = joinPath(joinPath(saves, sys), fileName);
        if (pathExists(p)) { result = p; break; }
      }
      // 2) gợi ý system từ peer
      if (result.empty() && !systemHint.empty() &&
          dirExists(joinPath(saves, systemHint)))
        result = joinPath(joinPath(saves, systemHint), fileName);
      // 3) hệ có ROM cùng tên
      if (result.empty()) {
        std::string stem = fileName;
        size_t dot = stem.rfind('.');
        if (dot != std::string::npos) stem = stem.substr(0, dot);
        for (const auto& sys : listSubdirs(saves)) {
          if (sys == "psp") continue;
          std::string romdir = joinPath(joinPath(root, "roms"), sys);
          bool hit = false;
          DIR* d = opendir(romdir.c_str());
          if (d) {
            struct dirent* e;
            while ((e = readdir(d)) != nullptr) {
              std::string n = e->d_name;
              if (n == stem || startsWith(n, stem + ".")) { hit = true; break; }
            }
            closedir(d);
          }
          if (hit) { result = joinPath(joinPath(saves, sys), fileName); break; }
        }
      }
      // 4) fallback: system đầu tiên
      if (result.empty()) {
        auto dirs = listSubdirs(saves);
        for (const auto& sys : dirs) {
          if (sys == "psp") continue;
          result = joinPath(joinPath(saves, sys), fileName);
          break;
        }
      }
    } else {
      // PSP: tail = "savedata/<GAMEID>/..." | "state/..."
      std::string kind = firstSegment(tail);
      std::string rest = tail.size() > kind.size() ? tail.substr(kind.size() + 1) : "";
      std::string gameId = firstSegment(rest);
      if (kind == "savedata") {
        std::vector<std::string> cands = {
          joinPath(root, "saves/psp/PSP/SAVEDATA"),
          joinPath(root, "saves/psp/SAVEDATA"),
        };
        result = joinPath(pickPspRoot(cands, gameId), rest);
        result = joinPath(result, fileName);
      } else if (kind == "state") {
        result = joinPath(joinPath(root, "saves/psp/PSP/PPSSPP_STATE"), rest);
        result = joinPath(result, fileName);
      }
    }
  } else {
    // ---- Stock layout ----
    if (isRa) {
      result = joinPath(joinPath(root, "RetroArch/.retroarch/saves"), fileName);
    } else {
      std::string kind = firstSegment(tail);
      std::string rest = tail.size() > kind.size() ? tail.substr(kind.size() + 1) : "";
      std::string gameId = firstSegment(rest);
      std::string sub = (kind == "savedata") ? "SAVEDATA"
                      : (kind == "state") ? "PPSSPP_STATE" : "";
      if (sub.empty()) return "";
      std::vector<std::string> cands;
      cands.push_back(joinPath(root, ".config/ppsspp/PSP/" + sub));
      for (const auto& ver : listSubdirs(joinPath(root, "Emus/PPSSPP"))) {
        std::string b = "Emus/PPSSPP/" + ver + "/.config/ppsspp/PSP/" + sub;
        if (dirExists(joinPath(root, b))) cands.push_back(joinPath(root, b));
      }
      result = joinPath(joinPath(pickPspRoot(cands, gameId), rest), fileName);
    }
  }
  if (result.empty()) return "";
  if (createDirs) {
    size_t p = result.rfind('/');
    if (p != std::string::npos) makeDirs(result.substr(0, p));
  }
  return result;
}

} // namespace RomCloud
