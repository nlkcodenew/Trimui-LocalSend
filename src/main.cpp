#include "AppConfig.h"
#include "LocalSendManager.h"
#include "Logger.h"

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

using namespace RomCloud;

namespace {

constexpr int kWidth = 1024;
constexpr int kHeight = 768;
constexpr int kHeaderHeight = 64;
constexpr int kFooterHeight = 53;

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
std::deque<LsUploadRequest> incomingRequests;

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
  const int top = kHeight - kFooterHeight;
  fillRect(0, top, kWidth, kFooterHeight, color(20, 24, 31));
  fillRect(0, top, kWidth, 1, color(55, 64, 78));
  drawText(left, 28, top + 15, color(220, 225, 232), fontSmall, 660);
  int width = 0;
  TTF_SizeUTF8(fontSmall, right.c_str(), &width, nullptr);
  drawText(right, kWidth - width - 28, top + 15, color(220, 225, 232), fontSmall);
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
  fillRect(0, 0, kWidth, kHeaderHeight, color(22, 27, 35));
  drawText("Trimui-LocalSend", 28, 17, color(246, 248, 252));
  const auto &manager = LocalSendManager::instance();
  std::string status = manager.alias() + "  |  " + manager.ownIp();
  int width = 0;
  TTF_SizeUTF8(fontSmall, status.c_str(), &width, nullptr);
  drawText(status, kWidth - width - 28, 22, color(145, 190, 255), fontSmall);
}

void drawHome() {
  const auto devices = LocalSendManager::instance().knownDevices();
  selectedDevice = std::min(selectedDevice, std::max(0, static_cast<int>(devices.size()) - 1));
  drawText("Thiết bị trong mạng", 40, 88, color(225, 230, 238));
  if (devices.empty()) {
    fillRect(40, 132, 944, 104, color(30, 36, 46));
    drawText("Chưa tìm thấy thiết bị LocalSend", 68, 154, color(242, 244, 248));
    drawText("Bật LocalSend trên máy khác và nhấn Y để quét lại.", 68, 194,
             color(150, 160, 176), fontSmall);
  } else {
    int y = 132;
    for (int index = 0; index < static_cast<int>(devices.size()) && index < 7; ++index) {
      const bool selected = index == selectedDevice;
      fillRect(40, y, 944, 68, selected ? color(44, 91, 150) : color(30, 36, 46));
      drawText(devices[index].alias, 64, y + 10, color(247, 249, 252), font, 580);
      const int64_t ageSec =
          (LsUtil::nowMs() - devices[index].lastSeenMs) / 1000;
      drawText(devices[index].deviceModel + "  " + devices[index].ip +
                   "  * " + std::to_string(ageSec) + "s truoc",
               64, y + 39, color(177, 189, 206), fontSmall, 820);
      y += 76;
    }
  }
  drawFooter("D-Pad Chọn   X Gửi file   Y Quét lại", "START Tiến trình   B Thoát");
}

void drawBrowser() {
  drawText("Chọn file để gửi", 40, 82, color(225, 230, 238));
  drawText(browserPath, 40, 116, color(145, 190, 255), fontSmall, 930);
  const int visible = 10;
  if (selectedEntry < browserScroll) browserScroll = selectedEntry;
  if (selectedEntry >= browserScroll + visible) browserScroll = selectedEntry - visible + 1;
  int y = 150;
  for (int row = 0; row < visible; ++row) {
    const int index = browserScroll + row;
    if (index >= static_cast<int>(browserEntries.size())) break;
    const bool selected = index == selectedEntry;
    fillRect(40, y, 944, 49, selected ? color(44, 91, 150) : color(30, 36, 46));
    const auto &entry = browserEntries[index];
    drawText(entry.directory ? "[THƯ MỤC]" : "FILE", 58, y + 13,
             entry.directory ? color(112, 205, 255) : color(139, 222, 174), fontSmall);
    drawText(entry.name, 190, y + 10, color(246, 248, 252), font, 620);
    if (!entry.directory) {
      drawText(LsUtil::humanSize(entry.size), 850, y + 14, color(177, 189, 206), fontSmall);
    }
    y += 54;
  }
  drawFooter("A Mở/Gửi   D-Pad Chọn", "B Quay lại");
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
  drawText("Tiến trình truyền file", 40, 84, color(225, 230, 238));
  auto sends = LocalSendManager::instance().sendProgresses();
  auto receives = LocalSendManager::instance().receiveProgresses();
  int y = 132;
  if (sends.empty() && receives.empty()) {
    drawText("Chưa có tác vụ.", 50, y, color(160, 170, 184));
  }
  for (const auto &item : sends) {
    if (y > 650) break;
    fillRect(40, y, 944, 78, color(30, 36, 46));
    drawText("GỬI", 58, y + 12, color(112, 205, 255), fontSmall);
    drawText(item.fileName, 130, y + 8, color(246, 248, 252), font, 580);
    drawText(sendStateName(item.state), 760, y + 12, color(139, 222, 174), fontSmall);
    const uint64_t total = std::max<uint64_t>(1, item.totalBytes);
    const int bar = static_cast<int>(760.0 * item.sentBytes / total);
    fillRect(130, y + 47, 760, 10, color(48, 57, 70));
    fillRect(130, y + 47, std::min(760, bar), 10, color(72, 156, 255));
    y += 86;
  }
  for (const auto &item : receives) {
    if (y > 650) break;
    fillRect(40, y, 944, 78, color(30, 36, 46));
    drawText("NHẬN", 58, y + 12, color(139, 222, 174), fontSmall);
    drawText(item.file.fileName, 130, y + 8, color(246, 248, 252), font, 580);
    drawText(receiveStateName(item.state), 760, y + 12, color(139, 222, 174), fontSmall);
    const uint64_t total = std::max<uint64_t>(1, item.file.size);
    const int bar = static_cast<int>(760.0 * item.receivedBytes / total);
    fillRect(130, y + 47, 760, 10, color(48, 57, 70));
    fillRect(130, y + 47, std::min(760, bar), 10, color(65, 196, 131));
    y += 86;
  }
  drawFooter("X Xóa tác vụ đã xong", "B Quay lại");
}

void drawIncoming() {
  LsUploadRequest request;
  {
    std::lock_guard<std::mutex> lock(incomingMutex);
    if (incomingRequests.empty()) return;
    request = incomingRequests.front();
  }
  fillRect(0, 0, kWidth, kHeight, color(0, 0, 0, 160));
  fillRect(122, 154, 780, 420, color(34, 40, 51));
  fillRect(122, 154, 780, 5, color(72, 156, 255));
  drawText("Yêu cầu nhận file", 166, 190, color(247, 249, 252));
  drawText("Từ: " + request.fromAlias + " (" + request.fromIp + ")", 166, 242,
           color(177, 189, 206), fontSmall, 690);
  drawText(request.file.fileName, 166, 292, color(247, 249, 252), font, 690);
  drawText("Kích thước: " + LsUtil::humanSize(request.file.size), 166, 338,
           color(177, 189, 206), fontSmall);
  drawText("Lưu vào: /mnt/SDCARD/Download/LocalSend", 166, 382,
           color(145, 190, 255), fontSmall);
  drawText("A  Chấp nhận", 212, 498, color(139, 222, 174));
  drawText("B  Từ chối", 602, 498, color(255, 139, 139));
}

void render() {
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  fillRect(0, 0, kWidth, kHeight, color(15, 18, 24));
  drawHeader();
  if (screen == Screen::Home) drawHome();
  else if (screen == Screen::Browser) drawBrowser();
  else drawTransfers();
  drawIncoming();
  SDL_RenderPresent(renderer);
}

bool init() {
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) {
    return false;
  }
  if (TTF_Init() != 0) return false;
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
  window = SDL_CreateWindow("Trimui-LocalSend", SDL_WINDOWPOS_CENTERED,
                            SDL_WINDOWPOS_CENTERED, kWidth, kHeight,
                            SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP);
  if (!window) return false;
  renderer = SDL_CreateRenderer(window, -1,
                                SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
  if (!renderer) return false;
  SDL_RenderSetLogicalSize(renderer, kWidth, kHeight);
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
  manager.setOnUserPrompt([](const LsUploadRequest &request) {
    std::lock_guard<std::mutex> lock(incomingMutex);
    incomingRequests.push_back(request);
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
    LsUploadRequest request;
    {
      std::lock_guard<std::mutex> lock(incomingMutex);
      incomingVisible = !incomingRequests.empty();
      if (incomingVisible) request = incomingRequests.front();
    }
    if (incomingVisible) {
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
      if (input.start) screen = Screen::Transfers;
      if (input.b) running = false;
    } else if (screen == Screen::Browser) {
      if (input.up) moveSelection(-1, static_cast<int>(browserEntries.size()), selectedEntry);
      if (input.down) moveSelection(1, static_cast<int>(browserEntries.size()), selectedEntry);
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
    } else {
      if (input.x) manager.clearFinishedTasks();
      if (input.b) screen = Screen::Home;
    }
    render();
    SDL_Delay(16);
  }

  shutdownApp();
  curl_global_cleanup();
  return 0;
}
