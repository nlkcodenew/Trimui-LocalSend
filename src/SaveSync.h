// SaveSync.h
// Quét và đồng bộ save game giữa 2 máy TrimUI (kể cả khác OS: stock OS
// và Knulli/Batocera có layout thư mục save khác nhau).
//
// Danh tính của 1 file save trên đường truyền là CANONICAL path, không
// phải đường dẫn vật lý của từng OS:
//   "ra/<file>.srm"              -> RetroArch battery save
//   "psp/savedata/<GAMEID>/..."  -> PPSSPP savedata
//   "psp/state/..."              -> PPSSPP save state
// Mỗi máy tự dịch canonical <-> vật lý theo layout của mình.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace RomCloud {

// Bố cục OS ảnh hưởng tới đường dẫn save vật lý
enum class SaveLayout { Stock, Knulli };

// Nhận diện layout 1 lần (cache). Override bằng env SAVE_LAYOUT=stock|knulli
// để test.
SaveLayout detectSaveLayout();
const char* saveLayoutName(SaveLayout l);

// Legacy rel (binary cũ gửi physical rel của stock) -> canonical.
// Canonical giữ nguyên.
std::string normalizeSaveRel(const std::string& rel);

struct SaveFileInfo {
  std::string relPath;   // CANONICAL rel dir, vd "psp/savedata/ULUS12345"
  std::string fileName;  // tên file
  uint64_t size = 0;
  int64_t mtime = 0;     // Unix timestamp
  std::string absPath;   // đường dẫn vật lý local
  std::string system;    // gợi ý hệ máy (Knulli RA save), rỗng nếu không có
};

class SaveSync {
public:
  // Quét tất cả thư mục save, trả về danh sách file (relPath = canonical)
  static std::vector<SaveFileInfo> scanLocalSaves(const std::string& root);

  // So sánh 2 danh sách, trả về file cần TẢI VỀ từ remote
  // (remote mới hơn hoặc local thiếu). Key so khớp: relPath + fileName.
  static std::vector<SaveFileInfo> diffForDownload(
      const std::vector<SaveFileInfo>& localList,
      const std::vector<SaveFileInfo>& remoteList);

  // Serialize/deserialize danh sách save sang JSON (gửi qua API).
  // fromJson tự normalize legacy rel -> canonical.
  static std::string toJson(const std::vector<SaveFileInfo>& list);
  static std::vector<SaveFileInfo> fromJson(const std::string& json);

  // Dịch canonical -> đường dẫn vật lý trên máy NÀY.
  // Trả về "" nếu không map được. createDirs=true: mkdir -p thư mục cha.
  static std::string resolveLocalPath(const std::string& root,
                                      const std::string& canonicalRel,
                                      const std::string& fileName,
                                      const std::string& systemHint = "",
                                      bool createDirs = false);
};

} // namespace RomCloud
