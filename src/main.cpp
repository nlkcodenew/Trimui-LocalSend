#include "AppConfig.h"
#include "LocalSendManager.h"
#include "Logger.h"
#include "OtaVerify.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <dirent.h>
#include <deque>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <set>
#include <map>

using namespace RomCloud;

namespace {

constexpr int kBaseW = 1024;
constexpr int kBaseH = 768;

// Kích thước màn hình thực tế — detect lúc chạy (theo cách chiaki-ng):
// TrimUI Brick Pro = 1024x768 (4:3), TrimUI Smart Pro S = 1280x720 (16:9).
// Mọi tọa độ UI được viết theo hệ cơ sở 1024x768 rồi scale qua X()/Y().
int scrW = kBaseW;
int scrH = kBaseH;

inline int X(int x) { return x * scrW / kBaseW; }
inline int Y(int y) { return y * scrH / kBaseH; }
inline int headerH() { return Y(64); }
inline int footerH() { return Y(53); }

enum class Screen { Home, Browser, Transfers };

struct BrowserEntry {
  std::string name;
  std::string path;
  bool directory = false;
  uint64_t size = 0;
};

struct InputState {
  bool up = false;
  bool down = false;
  bool left = false;
  bool right = false;
  bool a = false;
  bool b = false;
  bool x = false;
  bool y = false;
  bool start = false;
};

SDL_Window *window = nullptr;
SDL_Renderer *renderer = nullptr;
TTF_Font *font = nullptr;
TTF_Font *fontSmall = nullptr;
SDL_GameController *controller = nullptr;
SDL_Joystick *joystick = nullptr;
std::string appDirectory = ".";
std::atomic<bool> running{true};
Screen screen = Screen::Home;
int selectedDevice = 0;
int selectedEntry = 0;
int browserScroll = 0;
std::string browserPath = "/mnt/SDCARD";
std::vector<BrowserEntry> browserEntries;
std::mutex incomingMutex;
// ----- Folder send: thu thập đệ quy + dialog xác nhận -----
struct FolderFile {
  std::string absPath;
  std::string relativePath;  // vd "ten-thu-muc/sub/"
  uint64_t size = 0;
};
bool showFolderConfirm = false;
std::string folderConfirmPath;
// Toast thông báo (tự ẩn sau 3 giây, không cần bấm)
std::string toastMsg;
uint64_t toastUntilMs = 0;
void showToast(const std::string &msg) {
  toastMsg = msg;
  toastUntilMs = LsUtil::nowMs() + 3000;
}
std::string folderConfirmName;
std::vector<FolderFile> folderConfirmFiles;
uint64_t folderConfirmTotalBytes = 0;
// ----- Multi-select: X đánh dấu nhiều file, Y gửi tất cả 1 session -----
std::set<std::string> markedFiles;  // set absPath đã đánh dấu
std::deque<LsIncomingSession> incomingRequests;
// SaveSync 2 chiều: hàng đợi lời mời
std::deque<LsSyncInvite> syncInvites;
std::mutex syncInviteMutex;
std::atomic<bool> needShowTransfers{false};  // Bên nhận: tự chuyển sang màn Transfers khi có file đến

SDL_Color color(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha = 255) {
  return SDL_Color{red, green, blue, alpha};
}

void fillRect(int x, int y, int width, int height, SDL_Color value) {
  SDL_SetRenderDrawColor(renderer, value.r, value.g, value.b, value.a);
  SDL_Rect rect{x, y, width, height};
  SDL_RenderFillRect(renderer, &rect);
}

std::string truncateText(const std::string &text, TTF_Font *textFont,
                         int maxWidth) {
  int width = 0;
  if (TTF_SizeUTF8(textFont, text.c_str(), &width, nullptr) == 0 &&
      width <= maxWidth) {
    return text;
  }
  std::string output;
  size_t position = 0;
  while (position < text.size()) {
    size_t next = position + 1;
    const unsigned char lead = static_cast<unsigned char>(text[position]);
    if ((lead & 0xE0) == 0xC0) next = position + 2;
    else if ((lead & 0xF0) == 0xE0) next = position + 3;
    else if ((lead & 0xF8) == 0xF0) next = position + 4;
    next = std::min(next, text.size());
    const std::string candidate = output + text.substr(position, next - position) + "...";
    if (TTF_SizeUTF8(textFont, candidate.c_str(), &width, nullptr) != 0 ||
        width > maxWidth) {
      return output.empty() ? "..." : output + "...";
    }
    output.append(text, position, next - position);
    position = next;
  }
  return output;
}

void drawText(const std::string &text, int x, int y, SDL_Color value,
              TTF_Font *textFont = nullptr, int maxWidth = 0) {
  if (!textFont) textFont = font;
  const std::string shown = maxWidth > 0 ? truncateText(text, textFont, maxWidth) : text;
  SDL_Surface *surface = TTF_RenderUTF8_Blended(textFont, shown.c_str(), value);
  if (!surface) return;
  SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
  if (texture) {
    SDL_Rect destination{x, y, surface->w, surface->h};
    SDL_RenderCopy(renderer, texture, nullptr, &destination);
    SDL_DestroyTexture(texture);
  }
  SDL_FreeSurface(surface);
}

void drawFooter(const std::string &left, const std::string &right) {
  const int top = scrH - footerH();
  fillRect(0, top, scrW, footerH(), color(20, 24, 31));
  fillRect(0, top, scrW, std::max(1, Y(1)), color(55, 64, 78));
  drawText(left, X(28), top + Y(15), color(220, 225, 232), fontSmall, X(660));
  int width = 0;
  TTF_SizeUTF8(fontSmall, right.c_str(), &width, nullptr);
  drawText(right, scrW - width - X(28), top + Y(15), color(220, 225, 232), fontSmall);
}

std::string parentPath(const std::string &path) {
  if (path == "/mnt/SDCARD") return path;
  const size_t slash = path.find_last_of('/');
  if (slash == std::string::npos || slash < std::string("/mnt/SDCARD").size()) {
    return "/mnt/SDCARD";
  }
  return path.substr(0, slash);
}

void loadBrowser() {
  browserEntries.clear();
  DIR *directory = opendir(browserPath.c_str());
  if (!directory) return;
  dirent *entry = nullptr;
  while ((entry = readdir(directory)) != nullptr) {
    const std::string name = entry->d_name;
    if (name == "." || name == "..") continue;
    const std::string path = browserPath + "/" + name;
    struct stat status{};
    if (stat(path.c_str(), &status) != 0) continue;
    BrowserEntry item;
    item.name = name;
    item.path = path;
    item.directory = S_ISDIR(status.st_mode);
    item.size = item.directory ? 0 : static_cast<uint64_t>(status.st_size);
    browserEntries.push_back(std::move(item));
  }
  closedir(directory);
  std::sort(browserEntries.begin(), browserEntries.end(),
            [](const BrowserEntry &left, const BrowserEntry &right) {
              if (left.directory != right.directory) return left.directory;
              std::string leftName = left.name;
              std::string rightName = right.name;
              std::transform(leftName.begin(), leftName.end(), leftName.begin(), ::tolower);
              std::transform(rightName.begin(), rightName.end(), rightName.begin(), ::tolower);
              return leftName < rightName;
            });
  selectedEntry = 0;
  browserScroll = 0;
}

// Duyệt đệ quy thư mục, trả về danh sách file kèm relativePath.
// folderAbs: vd "/mnt/SDCARD/Roms/GBA", baseName: vd "GBA"
void collectFolderFiles(const std::string &folderAbs, const std::string &baseName,
                        const std::string &relPrefix, std::vector<FolderFile> &out) {
  DIR *dir = opendir(folderAbs.c_str());
  if (!dir) return;
  dirent *entry = nullptr;
  while ((entry = readdir(dir)) != nullptr) {
    const std::string name = entry->d_name;
    if (name == "." || name == "..") continue;
    const std::string path = folderAbs + "/" + name;
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      collectFolderFiles(path, baseName, relPrefix + name + "/", out);
    } else if (S_ISREG(st.st_mode)) {
      FolderFile ff;
      ff.absPath = path;
      ff.relativePath = baseName + "/" + relPrefix;
      ff.size = static_cast<uint64_t>(st.st_size);
      out.push_back(std::move(ff));
    }
  }
  closedir(dir);
}

// Chuẩn bị dialog xác nhận gửi thư mục: đếm file + tổng bytes (chính xác)
bool prepareFolderSend(const std::string &folderAbs) {
  folderConfirmFiles.clear();
  folderConfirmTotalBytes = 0;
  folderConfirmPath = folderAbs;
  // Lấy tên thư mục (basename)
  size_t pos = folderAbs.find_last_of('/');
  folderConfirmName = (pos == std::string::npos) ? folderAbs : folderAbs.substr(pos + 1);
  collectFolderFiles(folderAbs, folderConfirmName, "", folderConfirmFiles);
  for (const auto &ff : folderConfirmFiles) {
    folderConfirmTotalBytes += ff.size;
  }
  return !folderConfirmFiles.empty();
}

void moveSelection(int delta, int itemCount, int &selection) {
  if (itemCount <= 0) {
    selection = 0;
    return;
  }
  selection = (selection + delta + itemCount) % itemCount;
}

InputState pollInput() {
  InputState input;
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_QUIT) running = false;
    if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
      switch (event.key.keysym.sym) {
      case SDLK_UP: input.up = true; break;
      case SDLK_DOWN: input.down = true; break;
      case SDLK_LEFT: input.left = true; break;
      case SDLK_RIGHT: input.right = true; break;
      case SDLK_RETURN: case SDLK_a: input.a = true; break;
      case SDLK_ESCAPE: case SDLK_b: input.b = true; break;
      case SDLK_x: input.x = true; break;
      case SDLK_y: input.y = true; break;
      case SDLK_s: input.start = true; break;
      default: break;
      }
    }
    if (event.type == SDL_CONTROLLERBUTTONDOWN) {
      switch (event.cbutton.button) {
      case SDL_CONTROLLER_BUTTON_DPAD_UP: input.up = true; break;
      case SDL_CONTROLLER_BUTTON_DPAD_DOWN: input.down = true; break;
      case SDL_CONTROLLER_BUTTON_DPAD_LEFT: input.left = true; break;
      case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: input.right = true; break;
      case SDL_CONTROLLER_BUTTON_B: input.a = true; break;
      case SDL_CONTROLLER_BUTTON_A: input.b = true; break;
      case SDL_CONTROLLER_BUTTON_Y: input.x = true; break;
      case SDL_CONTROLLER_BUTTON_X: input.y = true; break;
      case SDL_CONTROLLER_BUTTON_START: input.start = true; break;
      default: break;
      }
    }
    if (event.type == SDL_JOYBUTTONDOWN) {
      switch (event.jbutton.button) {
      case 1: input.a = true; break;
      case 0: input.b = true; break;
      case 3: input.x = true; break;
      case 2: input.y = true; break;
      case 7: input.start = true; break;
      default: break;
      }
    }
    if (event.type == SDL_JOYHATMOTION) {
      input.up = (event.jhat.value & SDL_HAT_UP) != 0;
      input.down = (event.jhat.value & SDL_HAT_DOWN) != 0;
      input.left = (event.jhat.value & SDL_HAT_LEFT) != 0;
      input.right = (event.jhat.value & SDL_HAT_RIGHT) != 0;
    }
  }
  return input;
}

void drawHeader() {
  fillRect(0, 0, scrW, headerH(), color(22, 27, 35));
  drawText(std::string("Trimui-LocalSend v") + APP_VERSION, X(28), Y(17), color(246, 248, 252));
  const auto &manager = LocalSendManager::instance();
  std::string status = manager.alias() + "  |  " + manager.ownIp();
  int width = 0;
  TTF_SizeUTF8(fontSmall, status.c_str(), &width, nullptr);
  drawText(status, scrW - width - X(28), Y(22), color(145, 190, 255), fontSmall);
}

void drawHome() {
  const auto devices = LocalSendManager::instance().knownDevices();
  selectedDevice = std::min(selectedDevice, std::max(0, static_cast<int>(devices.size()) - 1));
  drawText("Thiết bị trong mạng", X(40), Y(88), color(225, 230, 238));
  if (devices.empty()) {
    fillRect(X(40), Y(132), X(944), Y(104), color(30, 36, 46));
    drawText("Chưa tìm thấy thiết bị LocalSend", X(68), Y(154), color(242, 244, 248));
    drawText("Bật LocalSend trên máy khác và nhấn Y để quét lại.", X(68), Y(194),
             color(150, 160, 176), fontSmall);
  } else {
    // Số dòng vừa với chiều cao thực tế (768p: 7 dòng, 720p: 8 dòng)
    int maxRows = (scrH - Y(132) - footerH() - Y(16)) / Y(76);
    if (maxRows < 1) maxRows = 1;
    if (maxRows > 12) maxRows = 12;
    int y = Y(132);
    for (int index = 0; index < static_cast<int>(devices.size()) && index < maxRows; ++index) {
      const bool selected = index == selectedDevice;
      fillRect(X(40), y, X(944), Y(68), selected ? color(44, 91, 150) : color(30, 36, 46));
      drawText(devices[index].alias, X(64), y + Y(10), color(247, 249, 252), font, X(580));
      const int64_t ageSec =
          (LsUtil::nowMs() - devices[index].lastSeenMs) / 1000;
      drawText(devices[index].deviceModel + "  " + devices[index].ip +
                   "  * " + std::to_string(ageSec) + "s truoc",
               X(64), y + Y(39), color(177, 189, 206), fontSmall, X(820));
      y += Y(76);
    }
  }
  drawFooter("D-Pad Chọn   A Đồng bộ Save   X Gửi file", "Y Quét lại   START Tiến trình   B Thoát");
}

void drawBrowser() {
  drawText("Chọn file để gửi", X(40), Y(82), color(225, 230, 238));
  drawText(browserPath, X(40), Y(116), color(145, 190, 255), fontSmall, X(930));
  int visible = (scrH - Y(150) - footerH() - Y(10)) / Y(54);
  if (visible < 4) visible = 4;
  if (visible > 16) visible = 16;
  if (selectedEntry < browserScroll) browserScroll = selectedEntry;
  if (selectedEntry >= browserScroll + visible) browserScroll = selectedEntry - visible + 1;
  int y = Y(150);
  for (int row = 0; row < visible; ++row) {
    const int index = browserScroll + row;
    if (index >= static_cast<int>(browserEntries.size())) break;
    const bool selected = index == selectedEntry;
    fillRect(X(40), y, X(944), Y(49), selected ? color(44, 91, 150) : color(30, 36, 46));
    const auto &entry = browserEntries[index];
    const bool isMarked = !entry.directory && markedFiles.count(entry.path);
    drawText(isMarked ? "[X]" : (entry.directory ? "[THƯ MỤC]" : "FILE"), X(58), y + Y(13),
             entry.directory ? color(112, 205, 255) : color(139, 222, 174), fontSmall);
    drawText(entry.name, X(190), y + Y(10), color(246, 248, 252), font, X(620));
    if (!entry.directory) {
      drawText(LsUtil::humanSize(entry.size), X(850), y + Y(14), color(177, 189, 206), fontSmall);
    }
    y += Y(54);
  }
  drawFooter("A Mở/Gửi   X Đánh dấu   Y Gửi đã chọn/thư mục", "B Quay lại");
  // Dialog xác nhận gửi thư mục (số file + dung lượng chính xác)
  if (showFolderConfirm) {
    const int dw = X(700), dh = Y(280);
    const int dx = (scrW - dw) / 2, dy = (scrH - dh) / 2;
    fillRect(dx, dy, dw, dh, color(24, 30, 42));
    fillRect(dx, dy, dw, 3, color(70, 130, 200));
    fillRect(dx, dy + dh - 3, dw, 3, color(70, 130, 200));
    fillRect(dx, dy, 3, dh, color(70, 130, 200));
    fillRect(dx + dw - 3, dy, 3, dh, color(70, 130, 200));
    drawText("Gửi thư mục?", dx + X(30), dy + Y(25), color(246, 248, 252));
    drawText(folderConfirmName, dx + X(30), dy + Y(65), color(112, 205, 255), font, X(600));
    char info[128];
    snprintf(info, sizeof(info), "%zu file - %s",
             folderConfirmFiles.size(),
             LsUtil::humanSize(folderConfirmTotalBytes).c_str());
    drawText(info, dx + X(30), dy + Y(115), color(177, 189, 206));
    drawText("A: Gửi    B: Hủy", dx + X(30), dy + Y(180), color(139, 222, 174));
  }
}

std::string sendStateName(LsSendProgress::State state) {
  switch (state) {
  case LsSendProgress::QUEUED: return "Đang chờ";
  case LsSendProgress::NEGOTIATING: return "Chờ chấp nhận";
  case LsSendProgress::UPLOADING: return "Đang gửi";
  case LsSendProgress::DONE: return "Hoàn tất";
  case LsSendProgress::FAILED: return "Lỗi";
  }
  return "";
}

std::string receiveStateName(LsUploadRequest::State state) {
  switch (state) {
  case LsUploadRequest::PENDING: return "Đang chờ";
  case LsUploadRequest::APPROVED: return "Đã chấp nhận";
  case LsUploadRequest::REJECTED: return "Đã từ chối";
  case LsUploadRequest::RECEIVING: return "Đang nhận";
  case LsUploadRequest::DONE: return "Hoàn tất";
  case LsUploadRequest::FAILED: return "Lỗi";
  }
  return "";
}

void drawTransfers() {
  drawText("Tiến trình truyền file", X(40), Y(84), color(225, 230, 238));
  // Panel trạng thái đồng bộ Save
  {
    auto st = LocalSendManager::instance().saveSyncStatus();
    if (st.active || (st.sendTotal > 0 || st.recvTotal > 0)) {
      int py = Y(108);
      fillRect(X(40), py, X(944), Y(64), color(28, 42, 38));
      std::string title = "Đồng bộ Save";
      if (!st.peerAlias.empty()) title += " với " + st.peerAlias;
      drawText(title, X(58), py + Y(8), color(139, 222, 174), fontSmall);
      // Đếm số file đã xong từ progress lists
      int sDone = 0, rDone = 0;
      {
        auto sends2 = LocalSendManager::instance().sendProgresses();
        auto receives2 = LocalSendManager::instance().receiveProgresses();
        for (const auto &s : sends2) if (s.state == LsSendProgress::DONE) sDone++;
        for (const auto &r : receives2) if (r.state == LsUploadRequest::DONE) rDone++;
      }
      std::string detail;
      // Chiều gửi: "không cần" nếu không có file, "đang thiếu" nếu đang gửi
      if (st.sendTotal == 0) detail += "Gửi: không cần";
      else if (sDone < st.sendTotal) detail += "Gửi: đang thiếu " + std::to_string(st.sendTotal - sDone) + " file";
      else detail += "Gửi: xong";
      detail += "   |   ";
      // Chiều nhận
      if (st.recvTotal == 0) detail += "Nhận: không cần";
      else if (rDone < st.recvTotal) detail += "Nhận: đang thiếu " + std::to_string(st.recvTotal - rDone) + " file";
      else detail += "Nhận: xong";
      if (!st.active) detail += "   (hoàn tất)";
      drawText(detail, X(58), py + Y(34), color(200, 210, 220), fontSmall);
    }
  }
  auto sends = LocalSendManager::instance().sendProgresses();
  auto receives = LocalSendManager::instance().receiveProgresses();
  const int bottomLimit = scrH - footerH() - Y(60);
  int y = Y(132);
  if (sends.empty() && receives.empty()) {
    drawText("Chưa có tác vụ.", X(50), y, color(160, 170, 184));
  }
  for (const auto &item : sends) {
    if (y > bottomLimit) break;
    fillRect(X(40), y, X(944), Y(78), color(30, 36, 46));
    drawText("GỬI", X(58), y + Y(12), color(112, 205, 255), fontSmall);
    drawText(item.fileName, X(130), y + Y(8), color(246, 248, 252), font, X(580));
    drawText(sendStateName(item.state), X(760), y + Y(12), color(139, 222, 174), fontSmall);
    const uint64_t total = std::max<uint64_t>(1, item.totalBytes);
    const int bar = static_cast<int>(X(760) * (double)item.sentBytes / (double)total);
    fillRect(X(130), y + Y(47), X(760), Y(10), color(48, 57, 70));
    fillRect(X(130), y + Y(47), std::min(X(760), bar), Y(10), color(72, 156, 255));
    y += Y(86);
  }
  // Gộp các file cùng session thành 1 thanh duy nhất (giống bên gửi, kiểu AirDrop)
  {
    std::map<std::string, std::vector<const LsUploadRequest*>> bySession;
    std::vector<std::string> order;
    for (const auto &item : receives) {
      if (bySession.find(item.sessionId) == bySession.end()) order.push_back(item.sessionId);
      bySession[item.sessionId].push_back(&item);
    }
    for (const auto &sid : order) {
      if (y > bottomLimit) break;
      const auto &group = bySession[sid];
      uint64_t total = 0, received = 0;
      std::string saveDir;
      LsUploadRequest::State st = LsUploadRequest::DONE;
      for (const auto *p : group) {
        total += p->file.size;
        received += p->receivedBytes;
        if (p->state != LsUploadRequest::DONE) st = p->state;
        if (!p->savedPath.empty() && saveDir.empty()) {
          size_t pos = p->savedPath.find_last_of('/');
          saveDir = (pos == std::string::npos) ? p->savedPath : p->savedPath.substr(0, pos);
        }
      }
      std::string displayName;
      if (group.size() > 1) {
        std::string rel = group[0]->file.relativePath;
        size_t slash = rel.find('/');
        std::string folder = (slash == std::string::npos) ? "" : rel.substr(0, slash);
        char buf[128];
        if (!folder.empty()) snprintf(buf, sizeof(buf), "%s (%zu file)", folder.c_str(), group.size());
        else snprintf(buf, sizeof(buf), "%zu file", group.size());
        displayName = buf;
      } else {
        displayName = group[0]->file.fileName;
      }
      fillRect(X(40), y, X(944), Y(100), color(30, 36, 46));
      drawText("NHẬN", X(58), y + Y(12), color(139, 222, 174), fontSmall);
      drawText(displayName, X(130), y + Y(8), color(246, 248, 252), font, X(580));
      drawText(receiveStateName(st), X(760), y + Y(12), color(139, 222, 174), fontSmall);
      const uint64_t tmax = std::max<uint64_t>(1, total);
      const int bar = static_cast<int>(X(760) * (double)received / (double)tmax);
      fillRect(X(130), y + Y(47), X(760), Y(10), color(48, 57, 70));
      fillRect(X(130), y + Y(47), std::min(X(760), bar), Y(10), color(65, 196, 131));
      if (!saveDir.empty()) {
        drawText(saveDir, X(130), y + Y(68), color(145, 190, 255), fontSmall, X(700));
      }
      y += Y(100) + Y(8);
    }
  }
  drawFooter("X Xóa tác vụ đã xong", "B Quay lại");
}

void drawIncoming() {
  LsIncomingSession session;
  {
    std::lock_guard<std::mutex> lock(incomingMutex);
    if (incomingRequests.empty()) return;
    session = incomingRequests.front();
  }
  // Hộp thoại căn giữa màn hình — vừa cả 4:3 (1024x768) lẫn 16:9 (1280x720)
  const int boxW = X(780);
  const int boxH = Y(420);
  const int bx = (scrW - boxW) / 2;
  const int by = (scrH - boxH) / 2;
  const int tx = bx + X(44);
  fillRect(0, 0, scrW, scrH, color(0, 0, 0, 160));
  fillRect(bx, by, boxW, boxH, color(34, 40, 51));
  fillRect(bx, by, boxW, std::max(1, Y(5)), color(72, 156, 255));
  drawText("Yêu cầu nhận file", tx, by + Y(36), color(247, 249, 252));
  drawText("Từ: " + session.fromAlias + " (" + session.fromIp + ")", tx, by + Y(88),
           color(177, 189, 206), fontSmall, X(690));
  if (session.files.size() == 1) {
    const auto &f = session.files[0];
    drawText(f.fileName, tx, by + Y(138), color(247, 249, 252), font, X(690));
    drawText("Kích thước: " + LsUtil::humanSize(f.size), tx, by + Y(184),
             color(177, 189, 206), fontSmall);
    drawText("Lưu vào: /mnt/SDCARD/Download/LocalSend", tx, by + Y(228),
             color(145, 190, 255), fontSmall);
  } else {
    // Nhiều file / thư mục: hiện số lượng + tổng dung lượng + vài tên file
    drawText(std::to_string(session.files.size()) + " files • Tổng: " +
             LsUtil::humanSize(session.totalSize), tx, by + Y(138),
             color(247, 249, 252), font, X(690));
    const size_t showN = std::min<size_t>(session.files.size(), 3);
    int y = by + Y(184);
    for (size_t i = 0; i < showN; ++i) {
      drawText("• " + session.files[i].fileName, tx, y,
               color(177, 189, 206), fontSmall, X(690));
      y += Y(34);
    }
    if (session.files.size() > showN) {
      drawText("…và " + std::to_string(session.files.size() - showN) + " file nữa",
               tx, y, color(177, 189, 206), fontSmall, X(690));
    }
    drawText("Lưu vào: /mnt/SDCARD/Download/LocalSend", tx, by + Y(300),
             color(145, 190, 255), fontSmall);
  }
  drawText("A  Chấp nhận", bx + X(90), by + Y(344), color(139, 222, 174));
  drawText("B  Từ chối", bx + X(480), by + Y(344), color(255, 139, 139));
}

void drawToast() {
  if (toastMsg.empty()) return;
  if (LsUtil::nowMs() > toastUntilMs) {
    toastMsg.clear();
    return;
  }
  // Vẽ toast ở giữa dưới màn hình
  const int boxW = X(600);
  const int boxH = Y(64);
  const int bx = (scrW - boxW) / 2;
  const int by = scrH - Y(140);
  fillRect(bx, by, boxW, boxH, color(40, 50, 65, 230));
  fillRect(bx, by, boxW, std::max(1, Y(4)), color(65, 196, 131));
  drawText(toastMsg, bx + X(24), by + Y(20), color(240, 245, 250), fontSmall, X(552));
}

void drawSyncInvite() {
  LsSyncInvite invite;
  {
    std::lock_guard<std::mutex> lock(syncInviteMutex);
    if (syncInvites.empty()) return;
    invite = syncInvites.front();
  }
  const int boxW = X(780);
  const int boxH = Y(360);
  const int bx = (scrW - boxW) / 2;
  const int by = (scrH - boxH) / 2;
  const int tx = bx + X(44);
  fillRect(0, 0, scrW, scrH, color(0, 0, 0, 160));
  fillRect(bx, by, boxW, boxH, color(34, 40, 51));
  fillRect(bx, by, boxW, std::max(1, Y(5)), color(65, 196, 131));
  drawText("Đồng bộ Save 2 chiều", tx, by + Y(36), color(247, 249, 252));
  drawText("Từ: " + invite.fromAlias + " (" + invite.fromIp + ")", tx, by + Y(88),
           color(177, 189, 206), fontSmall, X(690));
  drawText("Hai máy sẽ trao đổi save game mới nhất", tx, by + Y(138),
           color(247, 249, 252), fontSmall, X(690));
  drawText("cho nhau (PSP + RetroArch).", tx, by + Y(172),
           color(247, 249, 252), fontSmall, X(690));
  drawText("A  Đồng ý", bx + X(90), by + Y(284), color(139, 222, 174));
  drawText("B  Từ chối", bx + X(480), by + Y(284), color(255, 139, 139));
}

void render() {
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  fillRect(0, 0, scrW, scrH, color(15, 18, 24));
  drawHeader();
  if (screen == Screen::Home) drawHome();
  else if (screen == Screen::Browser) drawBrowser();
  else drawTransfers();
  drawIncoming();
  drawSyncInvite();
  drawToast();
  SDL_RenderPresent(renderer);
}

bool init() {
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) {
    return false;
  }
  // Detect độ phân giải thực của máy (chiaki-ng style). Brick Pro = 1024x768,
  // Smart Pro S = 1280x720. Fallback 1024x768 nếu không đọc được.
  scrW = kBaseW;
  scrH = kBaseH;
  SDL_DisplayMode dm;
  if (SDL_GetCurrentDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) {
    scrW = dm.w;
    scrH = dm.h;
  }
  if (scrW < 800)
    scrW = kBaseW;
  if (scrH < 600)
    scrH = kBaseH;
  Logger::info("UI: screen detected " + std::to_string(scrW) + "x" +
               std::to_string(scrH));
  if (TTF_Init() != 0) return false;
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
  window = SDL_CreateWindow("Trimui-LocalSend", SDL_WINDOWPOS_CENTERED,
                            SDL_WINDOWPOS_CENTERED, scrW, scrH,
                            SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP);
  if (!window) return false;
  renderer = SDL_CreateRenderer(window, -1,
                                SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
  if (!renderer) return false;
  SDL_RenderSetLogicalSize(renderer, scrW, scrH);
  const std::string fontPath = appDirectory + "/assets/NotoSans-Regular.ttf";
  font = TTF_OpenFont(fontPath.c_str(), 25);
  fontSmall = TTF_OpenFont(fontPath.c_str(), 18);
  if (!font || !fontSmall) return false;
  for (int index = 0; index < SDL_NumJoysticks(); ++index) {
    if (SDL_IsGameController(index)) {
      controller = SDL_GameControllerOpen(index);
      if (controller) break;
    }
  }
  if (!controller && SDL_NumJoysticks() > 0) joystick = SDL_JoystickOpen(0);
  return true;
}

void shutdownApp() {
  LocalSendManager::instance().stop();
  if (controller) SDL_GameControllerClose(controller);
  if (joystick) SDL_JoystickClose(joystick);
  if (font) TTF_CloseFont(font);
  if (fontSmall) TTF_CloseFont(fontSmall);
  if (renderer) SDL_DestroyRenderer(renderer);
  if (window) SDL_DestroyWindow(window);
  TTF_Quit();
  SDL_Quit();
}

} // namespace

int main(int argc, char **argv) {
  // Chế độ CLI cho OTA (không cần SDL): verify manifest ký Ed25519.
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--verify-manifest" && i + 1 < argc) {
      return OtaVerifyManifest(argv[i + 1]) ? 0 : 1;
    }
    if (arg == "--version") {
      printf("%s\n", APP_VERSION);
      return 0;
    }
  }
  if (argc > 0) {
    std::string executable = argv[0];
    const size_t slash = executable.find_last_of('/');
    if (slash != std::string::npos) appDirectory = executable.substr(0, slash);
    if (appDirectory.size() >= 4 && appDirectory.substr(appDirectory.size() - 4) == "/bin") {
      appDirectory.resize(appDirectory.size() - 4);
    }
  }
  AppConfig::instance().setDataDirectory(appDirectory + "/data");
  Logger::setLogPath(appDirectory + "/localsend.log");
  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
    Logger::error("Unable to initialize libcurl");
    return 1;
  }
  if (!init()) {
    Logger::error(std::string("SDL init failed: ") + SDL_GetError());
    shutdownApp();
    curl_global_cleanup();
    return 1;
  }

  auto &manager = LocalSendManager::instance();
  manager.setOnUserPrompt([](const LsIncomingSession &request) {
    if (request.isSaveSync) {
      // SaveSync đã auto-approve: chỉ hiện màn Transfers, không cần dialog
      needShowTransfers = true;
      return;
    }
    std::lock_guard<std::mutex> lock(incomingMutex);
    incomingRequests.push_back(request);
    needShowTransfers = true;
  });
  manager.setOnSyncInvite([](const LsSyncInvite &invite) {
    std::lock_guard<std::mutex> lock(syncInviteMutex);
    syncInvites.push_back(invite);
  });
  manager.setOnComplete([](const LsUploadRequest &) {});
  if (!manager.start()) {
    Logger::error("Unable to start LocalSend service");
  }
  manager.refreshDiscovery();

  while (running) {
    // pruneStalePrepares đã chạy mỗi 5s trong discoveryLoop — không gọi mỗi
    // frame để đỡ lock mutex vô ích.
    const InputState input = pollInput();
    bool incomingVisible = false;
    LsIncomingSession request;
    {
      std::lock_guard<std::mutex> lock(incomingMutex);
      incomingVisible = !incomingRequests.empty();
      if (incomingVisible) request = incomingRequests.front();
    }
    // Ưu tiên dialog sync invite trước
    bool syncVisible = false;
    LsSyncInvite syncInvite;
    {
      std::lock_guard<std::mutex> lock(syncInviteMutex);
      syncVisible = !syncInvites.empty();
      if (syncVisible) syncInvite = syncInvites.front();
    }
    if (syncVisible) {
      if (input.a || input.b) {
        if (input.a) {
          // Đồng ý: tìm device info và thực hiện sync 2 chiều
          auto devices = manager.knownDevices();
          for (const auto& d : devices) {
            if (d.ip == syncInvite.fromIp) {
              int n = manager.doTwoWaySync(d);
              if (n > 0) {
                screen = Screen::Transfers;
                showToast("Đang đồng bộ save...");
              } else if (n == 0) {
                showToast("Save đã khớp, không cần đồng bộ");
              } else {
                showToast("Lỗi đồng bộ save");
              }
              break;
            }
          }
          // Nếu không tìm thấy device, tạo tạm
          // (doTwoWaySync cần LsDeviceInfo)
        }
        // Xóa invite khỏi hàng đợi (dù đồng ý hay từ chối)
        std::lock_guard<std::mutex> lock(syncInviteMutex);
        if (!syncInvites.empty()) syncInvites.pop_front();
      }
    } else if (incomingVisible) {
      if (input.a || input.b) {
        if (input.a) manager.approveUploadWithPath(request.sessionId,
                                                   "/mnt/SDCARD/Download/LocalSend");
        else manager.rejectUpload(request.sessionId);
        std::lock_guard<std::mutex> lock(incomingMutex);
        if (!incomingRequests.empty()) incomingRequests.pop_front();
      }
    } else if (screen == Screen::Home) {
      auto devices = manager.knownDevices();
      if (input.up) moveSelection(-1, static_cast<int>(devices.size()), selectedDevice);
      if (input.down) moveSelection(1, static_cast<int>(devices.size()), selectedDevice);
      if (input.y) manager.refreshDiscovery();
      if (input.x && !devices.empty()) {
        browserPath = "/mnt/SDCARD";
        loadBrowser();
        screen = Screen::Browser;
      }
      // A: gửi lời mời đồng bộ save 2 chiều tới thiết bị đang chọn
      // Bên kia sẽ hiện dialog xác nhận, nếu đồng ý thì đồng bộ cả 2 chiều
      if (input.a && !devices.empty() &&
          selectedDevice >= 0 && selectedDevice < static_cast<int>(devices.size())) {
        if (manager.sendSyncInvite(devices[selectedDevice])) {
          // Đã gửi lời mời, chờ bên kia xác nhận
          // (Bên kia xác nhận xong sẽ tự bắt đầu đồng bộ 2 chiều)
        }
      }
      if (input.start) screen = Screen::Transfers;
      if (input.b) running = false;
    } else if (screen == Screen::Browser) {
      if (showFolderConfirm) {
        // Dialog xác nhận gửi thư mục: A = gửi, B = hủy
        if (input.a) {
          const auto devices = manager.knownDevices();
          if (!devices.empty() && selectedDevice < static_cast<int>(devices.size())
              && !folderConfirmFiles.empty()) {
            // Gửi cả thư mục trong 1 session duy nhất -> bên nhận chỉ hiện 1 dialog
            std::vector<std::pair<LsFileMeta, std::string>> batch;
            batch.reserve(folderConfirmFiles.size());
            for (const auto &ff : folderConfirmFiles) {
              LsFileMeta meta;
              meta.relativePath = ff.relativePath;
              batch.emplace_back(meta, ff.absPath);
            }
            manager.sendFilesAsync(batch, folderConfirmName, devices[selectedDevice]);
            showFolderConfirm = false;
            screen = Screen::Transfers;
          }
        } else if (input.b) {
          showFolderConfirm = false;
          folderConfirmFiles.clear();
        }
        // Không xử lý các nút khác khi dialog đang hiện
      } else {
      if (input.up) moveSelection(-1, static_cast<int>(browserEntries.size()), selectedEntry);
      if (input.down) moveSelection(1, static_cast<int>(browserEntries.size()), selectedEntry);
      // X: đánh dấu/bỏ đánh dấu file (để gửi nhiều file 1 lần)
      if (input.x && !browserEntries.empty()) {
        const BrowserEntry entry = browserEntries[selectedEntry];
        if (!entry.directory) {
          if (markedFiles.count(entry.path)) markedFiles.erase(entry.path);
          else markedFiles.insert(entry.path);
        }
      }
      // Y: gửi tất cả file đã đánh dấu (1 session) hoặc gửi cả thư mục đang chọn
      if (input.y && !browserEntries.empty()) {
        if (!markedFiles.empty()) {
          // Gửi tất cả file đã đánh dấu trong 1 session duy nhất
          const auto devices = manager.knownDevices();
          if (!devices.empty() && selectedDevice < static_cast<int>(devices.size())) {
            std::vector<std::pair<LsFileMeta, std::string>> batch;
            batch.reserve(markedFiles.size());
            for (const auto &p : markedFiles) {
              LsFileMeta meta;
              batch.emplace_back(meta, p);
            }
            manager.sendFilesAsync(batch, "Đã chọn (" + std::to_string(batch.size()) + " file)",
                                   devices[selectedDevice]);
            markedFiles.clear();
            screen = Screen::Transfers;
          }
        } else {
          const BrowserEntry entry = browserEntries[selectedEntry];
          if (entry.directory) {
            if (prepareFolderSend(entry.path)) {
              showFolderConfirm = true;
            }
          }
        }
      }
      if (input.a && !browserEntries.empty()) {
        const BrowserEntry entry = browserEntries[selectedEntry];
        if (entry.directory) {
          browserPath = entry.path;
          loadBrowser();
        } else {
          const auto devices = manager.knownDevices();
          if (!devices.empty() && selectedDevice < static_cast<int>(devices.size())) {
            manager.sendFileMetaAsync({}, entry.path, devices[selectedDevice]);
            screen = Screen::Transfers;
          }
        }
      }
      if (input.b) {
        if (browserPath == "/mnt/SDCARD") screen = Screen::Home;
        else {
          browserPath = parentPath(browserPath);
          loadBrowser();
        }
      }
      } // end else (không phải folder confirm dialog)
    } else {
      if (input.x) manager.clearFinishedTasks();
      if (input.b) screen = Screen::Home;
    }
    if (needShowTransfers.exchange(false)) {
      screen = Screen::Transfers;
    }
    render();
    SDL_Delay(16);
  }

  shutdownApp();
  curl_global_cleanup();
  return 0;
}
