// SaveSync.cpp
// Quét và đồng bộ save game giữa 2 máy TrimUI.

#include "SaveSync.h"
#include <dirent.h>
#include <sys/stat.h>
#include <cstring>

namespace RomCloud {

static void scanDirRecursive(const std::string& baseDir, const std::string& relDir,
                             std::vector<SaveFileInfo>& out) {
  std::string fullPath = baseDir + "/" + relDir;
  DIR* dir = opendir(fullPath.c_str());
  if (!dir) return;

  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    std::string childRel = relDir.empty() ? entry->d_name : relDir + "/" + entry->d_name;
    std::string childFull = baseDir + "/" + childRel;

    struct stat st;
    if (stat(childFull.c_str(), &st) != 0) continue;

    if (S_ISDIR(st.st_mode)) {
      scanDirRecursive(baseDir, childRel, out);
    } else if (S_ISREG(st.st_mode)) {
      SaveFileInfo info;
      info.relPath = relDir;
      info.fileName = entry->d_name;
      info.size = static_cast<uint64_t>(st.st_size);
      info.mtime = static_cast<int64_t>(st.st_mtime);
      info.absPath = childFull;
      out.push_back(info);
    }
  }
  closedir(dir);
}

std::vector<SaveFileInfo> SaveSync::scanLocalSaves(const std::string& sdcardRoot) {
  std::vector<SaveFileInfo> out;
  const char* dirs[] = {
    SaveDirs::kPspSaveData,
    SaveDirs::kPspSaveState,
    SaveDirs::kRetroArchSaves,
  };
  for (const char* d : dirs) {
    scanDirRecursive(sdcardRoot, d, out);
  }
  // Quét thêm các bản PPSSPP portable (mỗi version 1 folder)
  scanPortableSaves(sdcardRoot, out);
  return out;
}

void SaveSync::scanPortableSaves(const std::string& sdcardRoot,
                                 std::vector<SaveFileInfo>& out) {
  // PPSSPP portable: Emus/PPSSPP/<version>/.config/ppsspp/PSP/{SAVEDATA,PPSSPP_STATE}
  // relPath giữ nguyên cấu trúc đầy đủ để bên nhận lưu đúng chỗ
  std::string emuDir = sdcardRoot + "/Emus/PPSSPP";
  DIR* dir = opendir(emuDir.c_str());
  if (!dir) return;
  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    if (entry->d_name[0] == '.') continue;
    std::string verPath = std::string(entry->d_name) + "/.config/ppsspp/PSP";
    std::string fullVer = emuDir + "/" + verPath;
    struct stat st;
    if (stat(fullVer.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
    // Quét SAVEDATA và PPSSPP_STATE trong folder portable này
    std::string relBase = "Emus/PPSSPP/" + verPath;
    scanDirRecursive(sdcardRoot, relBase + "/SAVEDATA", out);
    scanDirRecursive(sdcardRoot, relBase + "/PPSSPP_STATE", out);
  }
  closedir(dir);
}

std::vector<SaveFileInfo> SaveSync::diffForDownload(
    const std::vector<SaveFileInfo>& localList,
    const std::vector<SaveFileInfo>& remoteList) {
  std::vector<SaveFileInfo> needDownload;

  // Map local theo "relPath/fileName" -> info
  for (const auto& remote : remoteList) {
    std::string key = remote.relPath + "/" + remote.fileName;
    const SaveFileInfo* localMatch = nullptr;
    for (const auto& local : localList) {
      std::string lkey = local.relPath + "/" + local.fileName;
      if (lkey == key) {
        localMatch = &local;
        break;
      }
    }
    if (!localMatch) {
      // Local chưa có -> cần tải
      needDownload.push_back(remote);
    } else if (remote.mtime > localMatch->mtime + 2) {
      // Remote mới hơn (dung sai 2 giây) -> cần tải
      // (Hoặc size khác cũng tính)
      needDownload.push_back(remote);
    } else if (remote.size != localMatch->size &&
               remote.mtime >= localMatch->mtime) {
      needDownload.push_back(remote);
    }
  }
  return needDownload;
}

// JSON đơn giản (không dùng lib): [{"rel":"...","name":"...","size":123,"mtime":456},...]
std::string SaveSync::toJson(const std::vector<SaveFileInfo>& list) {
  std::string json = "[";
  for (size_t i = 0; i < list.size(); ++i) {
    if (i > 0) json += ",";
    const auto& f = list[i];
    json += "{\"rel\":\"" + f.relPath + "\",\"name\":\"" + f.fileName + "\",";
    json += "\"size\":" + std::to_string(f.size) + ",";
    json += "\"mtime\":" + std::to_string(f.mtime) + "}";
  }
  json += "]";
  return json;
}

// Parse JSON đơn giản (giả định format do toJson tạo ra)
std::vector<SaveFileInfo> SaveSync::fromJson(const std::string& json) {
  std::vector<SaveFileInfo> out;
  size_t pos = 0;
  while ((pos = json.find("{\"rel\":\"", pos)) != std::string::npos) {
    SaveFileInfo info;
    pos += 8; // len("{\"rel\":\"")
    size_t end = json.find("\"", pos);
    info.relPath = json.substr(pos, end - pos);
    pos = json.find("\"name\":\"", end) + 8;
    end = json.find("\"", pos);
    info.fileName = json.substr(pos, end - pos);
    pos = json.find("\"size\":", end) + 7;
    end = json.find(",", pos);
    info.size = std::stoull(json.substr(pos, end - pos));
    pos = json.find("\"mtime\":", end) + 8;
    end = json.find("}", pos);
    info.mtime = std::stoll(json.substr(pos, end - pos));
    // absPath sẽ được set khi nhận file (dựa vào relPath)
    out.push_back(info);
    pos = end;
  }
  return out;
}

} // namespace RomCloud
