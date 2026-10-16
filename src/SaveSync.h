// SaveSync.h
// Quét và đồng bộ save game giữa 2 máy TrimUI.
// Hỗ trợ: PPSSPP (SAVEDATA, PPSSPP_STATE), RetroArch (.srm)

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace RomCloud {

// Các thư mục save chuẩn trên TrimUI (relative từ /mnt/SDCARD)
namespace SaveDirs {
constexpr const char* kPspSaveData = ".config/ppsspp/PSP/SAVEDATA";
constexpr const char* kPspSaveState = ".config/ppsspp/PSP/PPSSPP_STATE";
constexpr const char* kRetroArchSaves = "RetroArch/.retroarch/saves";
} // namespace SaveDirs

struct SaveFileInfo {
  std::string relPath;   // vd ".config/ppsspp/PSP/SAVEDATA/ULUS12345/"
  std::string fileName;  // tên file
  uint64_t size = 0;
  int64_t mtime = 0;     // Unix timestamp
  std::string absPath;   // đường dẫn tuyệt đối local
};

class SaveSync {
public:
  // Quét tất cả thư mục save, trả về danh sách file
  static std::vector<SaveFileInfo> scanLocalSaves(const std::string& sdcardRoot);

  // So sánh 2 danh sách, trả về file cần đồng bộ (bên remote mới hơn hoặc local thiếu)
  // localList: danh sách của máy mình, remoteList: danh sách của máy kia
  // Trả về: file cần TẢI VỀ từ remote (remote mới hơn hoặc local không có)
  static std::vector<SaveFileInfo> diffForDownload(
      const std::vector<SaveFileInfo>& localList,
      const std::vector<SaveFileInfo>& remoteList);

  // Serialize/deserialize danh sách save sang JSON (để gửi qua API)
  static std::string toJson(const std::vector<SaveFileInfo>& list);
  static std::vector<SaveFileInfo> fromJson(const std::string& json);
};

} // namespace RomCloud
