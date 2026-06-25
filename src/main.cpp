#include <Arduino.h>
#include <cctype>
#include <cstring>
#include <M5Cardputer.h>
#include <SD.h>
#include <SPI.h>

#include "AppManager.h"
#include "Apps.h"
#include "Features.h"
#include "InputManager.h"
#include "NetworkManager.h"
#include "PowerManager.h"
#include "SettingsManager.h"
#include "StorageManager.h"
#include "TerminalUI.h"

#if FEATURE_ULTRA_SAFE_BOOT
namespace {
constexpr uint8_t kSdSck = 40;
constexpr uint8_t kSdMiso = 39;
constexpr uint8_t kSdMosi = 14;
constexpr uint8_t kSdCs = 12;
constexpr uint32_t kSdSpeeds[] = {400000UL, 1000000UL, 4000000UL, 10000000UL, 25000000UL};
constexpr uint32_t kBootHeartbeatMs = 2000;
constexpr uint32_t kStateHeartbeatMs = 1000;
constexpr uint32_t kInputDebounceMs = 80;
constexpr uint32_t kBtnLongPressMs = 700;
constexpr uint32_t kBtnDebounceMs = 80;

// NOTE: keep safe-mode enums and state centralized to avoid AppManager/UI dependencies.
enum class SafeModeScreen : uint8_t {
  BootDiag,
  SafeLauncher,
  SafeInputTest,
  SafeSystemInfo,
  SafeRandomizer,
  SafeClock,
  SafeNotesList,
  SafeReaderList,
  SafeMusicStatus,
  SafeDisabledApp
};

enum class SafeSdMountState : uint8_t {
  NotTested,
  Ok,
  Failed
};

struct SafeLauncherItem {
  const char* title;
  const char* icon;
  SafeModeScreen target;
  bool requiresSd;
  bool disabled;
  const char* disabledMessage;
};

struct SafeSdState {
  SafeSdMountState mountState = SafeSdMountState::NotTested;
  uint8_t cardType = 0;
  uint64_t cardSizeBytes = 0;
  uint32_t testedSpeed = 0;
  String lastError;
};

enum class SafeAction : uint8_t {
  None,
  GoShort,
  GoLong,
  Enter,
  Back,
  Launcher,
  Next,
  Prev,
  Up,
  Down,
  RetrySD,
  Boot,
  Right,
  Text
};

struct SafeInputState {
  char lastRaw[16] = {0};
  SafeAction lastAction = SafeAction::None;
  bool goShort = false;
  bool goLong = false;
  bool enter = false;
  bool backspace = false;
  uint32_t heartbeat = 0;
};

constexpr uint16_t kColorBg = 0x0000;
constexpr uint16_t kColorWhite = 0xFFFF;
constexpr uint16_t kColorYellow = 0xFFE0;
constexpr uint16_t kColorCyan = 0x07FF;

SafeSdState g_sdState;
SafeModeScreen g_safeScreen = SafeModeScreen::BootDiag;
SafeModeScreen g_lastSafeScreen = SafeModeScreen::BootDiag;
uint8_t g_safeSelectedApp = 0;
bool g_needRedraw = true;
bool g_btnDown = false;
uint32_t g_btnDownAtMs = 0;
bool g_btnLongHandled = false;
uint32_t g_lastBtnChangeMs = 0;
uint32_t g_lastHeartbeatMs = 0;
uint32_t g_lastStateRedrawMs = 0;
uint32_t g_lastKeyboardEventMs = 0;
SafeInputState g_inputState;
String g_randomResult = "Maybe";
String g_disabledMessage;
bool g_keyboardConsumedChange = false;

const SafeLauncherItem kSafeLauncherItems[] = {
  {"System Info", "II", SafeModeScreen::SafeSystemInfo, false, false, ""},
  {"Input Test", "TT", SafeModeScreen::SafeInputTest, false, false, ""},
  {"SD Test", "SD", SafeModeScreen::BootDiag, false, false, "Run SD test"},
  {"Notes", "NT", SafeModeScreen::SafeNotesList, true, false, "SD required"},
  {"Reader", "RD", SafeModeScreen::SafeReaderList, true, false, "SD required"},
  {"Music", "MU", SafeModeScreen::SafeMusicStatus, true, false, "SD required"},
  {"Randomizer", "RND", SafeModeScreen::SafeRandomizer, false, false, ""},
  {"Clock", "CLK", SafeModeScreen::SafeClock, false, false, ""},
  {"Disabled Apps", "OFF", SafeModeScreen::SafeDisabledApp, false, true, "Disabled in v0.1.5 safe mode"}
};

constexpr uint8_t kSafeLauncherCount = sizeof(kSafeLauncherItems) / sizeof(kSafeLauncherItems[0]);

bool isHiddenFileName(const String& name) {
  return name.startsWith(".") || name.startsWith("._") || name.equals(".DS_Store");
}

const char* actionName(SafeAction action);
const char* screenName(SafeModeScreen screen);
void formatRawLabelForLog(char* out, size_t outSize, char c);
void logSafeInput(const char* raw, SafeAction action);

void drawBootHeader(const char* sdLine, const char* bottomHint) {
  const int16_t w = M5Cardputer.Display.width();
  const int16_t h = M5Cardputer.Display.height();
  M5Cardputer.Display.fillScreen(kColorBg);
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setTextColor(kColorWhite, kColorBg);
  M5Cardputer.Display.setCursor(8, 8);
  M5Cardputer.Display.println("CARDPUTER ABVx");
  M5Cardputer.Display.println("v0.1.5 SAFE BOOT");
  M5Cardputer.Display.println("Serial: OK");
  M5Cardputer.Display.println("Display: OK");
  M5Cardputer.Display.println(sdLine);
  M5Cardputer.Display.setTextColor(kColorYellow, kColorBg);
  M5Cardputer.Display.println(bottomHint);
  M5Cardputer.Display.setCursor(8, h - 20);
  M5Cardputer.Display.println("1: safe launcher");
}

void drawBootDiag() {
  const int16_t h = M5Cardputer.Display.height();
  const char* sdLine = "SD: not tested";
  if (g_sdState.mountState == SafeSdMountState::Ok) sdLine = "SD: OK";
  else if (g_sdState.mountState == SafeSdMountState::Failed) sdLine = "SD: failed";
  drawBootHeader(sdLine, "GO: test SD");
  M5Cardputer.Display.setTextColor(kColorCyan, kColorBg);
  M5Cardputer.Display.setCursor(8, h - 10);
}

void drawTopBar(const char* title, const char* status) {
  const int16_t w = M5Cardputer.Display.width();
  M5Cardputer.Display.fillRect(0, 0, w, 14, kColorBg);
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setTextColor(kColorWhite, kColorBg);
  M5Cardputer.Display.setCursor(2, 2);
  M5Cardputer.Display.print(title);
  const char* safeStatus = status ? status : "-";
  const int rightLen = strlen(safeStatus) * 6;
  int16_t rightX = w - rightLen - 2;
  if (rightX < 0) rightX = 0;
  M5Cardputer.Display.setCursor(rightX, 2);
  M5Cardputer.Display.print(safeStatus);
  M5Cardputer.Display.drawFastHLine(0, 13, w, kColorCyan);
}

void drawSafeFooter(const char* line1) {
  const int16_t w = M5Cardputer.Display.width();
  const int16_t h = M5Cardputer.Display.height();
  M5Cardputer.Display.fillRect(0, h - 16, w, 16, kColorBg);
  M5Cardputer.Display.drawFastHLine(0, h - 17, w, kColorCyan);
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setTextColor(kColorWhite, kColorBg);
  M5Cardputer.Display.setCursor(2, h - 13);
  M5Cardputer.Display.print(line1);
}

void drawLauncherScreen() {
  const int16_t w = M5Cardputer.Display.width();
  const int16_t h = M5Cardputer.Display.height();

  const SafeLauncherItem& item = kSafeLauncherItems[g_safeSelectedApp];
  char status[8];
  snprintf(status, sizeof(status), "%u/%u", static_cast<unsigned>(g_safeSelectedApp + 1), static_cast<unsigned>(kSafeLauncherCount));

  M5Cardputer.Display.fillScreen(kColorBg);
  drawTopBar("ABVx SAFE", status);

  M5Cardputer.Display.setTextSize(3);
  M5Cardputer.Display.setTextColor(kColorYellow, kColorBg);
  int cx = (w - static_cast<int16_t>(String(item.icon).length()) * 18) / 2;
  if (cx < 0) cx = 0;
  M5Cardputer.Display.setCursor(cx, h / 2 - 28);
  M5Cardputer.Display.println(item.icon);

  M5Cardputer.Display.setTextSize(2);
  M5Cardputer.Display.setTextColor(kColorWhite, kColorBg);
  const char* titlePtr = item.title;
  uint8_t titleLen = static_cast<uint8_t>(strlen(titlePtr));
  if (titleLen > 18) {
    titlePtr = item.title;
    titleLen = 18;
  }
  int tx = (w - static_cast<int16_t>(titleLen) * 12) / 2;
  if (tx < 0) tx = 0;
  M5Cardputer.Display.setCursor(tx, h / 2 + 6);
  M5Cardputer.Display.println(String(item.title).substring(0, titleLen));

  drawSafeFooter("< > APP   GO:OPEN   0:BOOT");
}

void drawSafeMusicList() {
  M5Cardputer.Display.fillScreen(kColorBg);
  drawTopBar("Music Status", "6/9");

  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setTextColor(kColorWhite, kColorBg);
  M5Cardputer.Display.setCursor(2, 20);

  if (g_sdState.mountState != SafeSdMountState::Ok) {
    M5Cardputer.Display.println("SD not mounted");
    M5Cardputer.Display.println("insert card + SD test");
    drawSafeFooter("0 BOOT");
    return;
  }

  String files[3];
  uint8_t count = 0;
  File root = SD.open("/music");
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    M5Cardputer.Display.println("/music missing");
    drawSafeFooter("R RE-SCAN  0 BOOT");
    return;
  }

  for (File f = root.openNextFile(); f && count < 3; f = root.openNextFile()) {
    if (!f) {
      break;
    }

    if (f.isDirectory()) {
      f.close();
      continue;
    }

    String name = f.name();
    String lower = name;
    lower.toLowerCase();
    if (!lower.endsWith(".mp3")) {
      f.close();
      continue;
    }
    if (isHiddenFileName(name)) {
      f.close();
      continue;
    }

    files[count++] = name;
    f.close();
  }
  root.close();

  M5Cardputer.Display.print("Found ");
  M5Cardputer.Display.print(count);
  M5Cardputer.Display.println(" MP3 file(s)");
  for (uint8_t i = 0; i < count; ++i) {
    M5Cardputer.Display.print("- ");
    M5Cardputer.Display.println(files[i]);
  }
  drawSafeFooter("R RESCAN   GO/B=BACK");
}

void drawSafeSimpleListScreen(const char* title, const char* dirPath, const char* emptyLine) {
  M5Cardputer.Display.fillScreen(kColorBg);
  if (strcmp(title, "Notes") == 0) {
    drawTopBar(title, "4/9");
  } else {
    drawTopBar(title, "5/9");
  }
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setTextColor(kColorWhite, kColorBg);
  M5Cardputer.Display.setCursor(2, 20);

  if (g_sdState.mountState != SafeSdMountState::Ok) {
    M5Cardputer.Display.println("SD not mounted");
    M5Cardputer.Display.println("run SD test first");
    drawSafeFooter("0 BOOT");
    return;
  }

  File root = SD.open(dirPath);
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    M5Cardputer.Display.println(emptyLine ? emptyLine : "Missing folder");
    M5Cardputer.Display.println("No files available");
    drawSafeFooter("0 BOOT");
    return;
  }

  uint8_t shown = 0;
  String names[8];
  for (File f = root.openNextFile(); f && shown < 8; f = root.openNextFile()) {
    if (!f) break;
    if (f.isDirectory()) {
      f.close();
      continue;
    }
    String name = f.name();
    String lower = name;
    lower.toLowerCase();
    if (!lower.endsWith(".txt") || isHiddenFileName(name)) {
      f.close();
      continue;
    }
    names[shown++] = name;
    f.close();
  }
  root.close();

  if (!shown) {
    M5Cardputer.Display.println(emptyLine);
    drawSafeFooter("0 BOOT");
    return;
  }

  M5Cardputer.Display.print("Found ");
  M5Cardputer.Display.print(shown);
  M5Cardputer.Display.println(" files:");
  for (uint8_t i = 0; i < shown; ++i) {
    M5Cardputer.Display.print("- ");
    M5Cardputer.Display.println(names[i]);
  }

  drawSafeFooter("0 BOOT");
}

void drawSafeInputTest() {
  M5Cardputer.Display.fillScreen(kColorBg);
  drawTopBar("Input Test", "2/9");
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setTextColor(kColorWhite, kColorBg);
  M5Cardputer.Display.setCursor(2, 20);
  M5Cardputer.Display.print("Raw: ");
  M5Cardputer.Display.println(g_inputState.lastRaw[0] ? g_inputState.lastRaw : "-");
  M5Cardputer.Display.print("Action: ");
  M5Cardputer.Display.println(actionName(g_inputState.lastAction));
  M5Cardputer.Display.print("GO short: ");
  M5Cardputer.Display.println(g_inputState.goShort ? "yes" : "no");
  M5Cardputer.Display.print("GO long: ");
  M5Cardputer.Display.println(g_inputState.goLong ? "yes" : "no");
  M5Cardputer.Display.print("Enter: ");
  M5Cardputer.Display.println(g_inputState.enter ? "yes" : "no");
  M5Cardputer.Display.print("Backspace: ");
  M5Cardputer.Display.println(g_inputState.backspace ? "yes" : "no");
  M5Cardputer.Display.print("HB: ");
  M5Cardputer.Display.print(g_inputState.heartbeat);
  drawSafeFooter("GO/B=BACK   0 BOOT");
}

void drawSafeSystemInfo() {
  M5Cardputer.Display.fillScreen(kColorBg);
  drawTopBar("System Info", "1/9");
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setTextColor(kColorWhite, kColorBg);
  M5Cardputer.Display.setCursor(2, 20);
  M5Cardputer.Display.println(String("fw: ") + FIRMWARE_VERSION);
  M5Cardputer.Display.print("heap: ");
  M5Cardputer.Display.println(ESP.getFreeHeap());
  M5Cardputer.Display.print("uptime: ");
  M5Cardputer.Display.print(millis() / 1000);
  M5Cardputer.Display.println(" s");

  if (g_sdState.mountState == SafeSdMountState::NotTested) {
    M5Cardputer.Display.println("SD: not tested");
  } else if (g_sdState.mountState == SafeSdMountState::Failed) {
    M5Cardputer.Display.println("SD: failed");
    if (g_sdState.lastError.length()) {
      M5Cardputer.Display.println(g_sdState.lastError);
    }
  } else {
    M5Cardputer.Display.println("SD: OK");
    M5Cardputer.Display.print("type: ");
    M5Cardputer.Display.println(g_sdState.cardType);
    M5Cardputer.Display.print("sizeMB: ");
    M5Cardputer.Display.println(g_sdState.cardSizeBytes / (1024ULL * 1024ULL));
    M5Cardputer.Display.print("speed: ");
    M5Cardputer.Display.print(g_sdState.testedSpeed);
    M5Cardputer.Display.println(" Hz");
  }
  M5Cardputer.Display.println("Serial OK");
  M5Cardputer.Display.println("Display OK");
  drawSafeFooter("R RETRY SD   GO/B/0 BACK");
}

void drawSafeRandomizer() {
  M5Cardputer.Display.fillScreen(kColorBg);
  drawTopBar("Randomizer", "7/9");
  M5Cardputer.Display.setTextSize(2);
  M5Cardputer.Display.setTextColor(kColorYellow, kColorBg);
  M5Cardputer.Display.setCursor(2, 30);
  M5Cardputer.Display.println("Simple roll:");
  M5Cardputer.Display.setTextSize(3);
  M5Cardputer.Display.setTextColor(kColorWhite, kColorBg);
  M5Cardputer.Display.setCursor(2, 62);
  M5Cardputer.Display.println(g_randomResult);
  drawSafeFooter("GO ROLL   GO/B/0 BACK");
}

void drawSafeClock() {
  M5Cardputer.Display.fillScreen(kColorBg);
  drawTopBar("Clock", "8/9");
  M5Cardputer.Display.setTextSize(2);
  M5Cardputer.Display.setTextColor(kColorCyan, kColorBg);
  M5Cardputer.Display.setCursor(10, 40);
  uint32_t seconds = millis() / 1000;
  uint32_t m = seconds / 60;
  uint32_t h = m / 60;
  uint32_t s = seconds % 60;
  M5Cardputer.Display.printf("UPTIME\n%02lu:%02lu:%02lu", h % 100, m % 60, s);
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setTextColor(kColorWhite, kColorBg);
  M5Cardputer.Display.setCursor(10, 100);
  M5Cardputer.Display.println("No timer/beep in safe mode");
  drawSafeFooter("GO/0/B BACK");
}

void drawSafeDisabledApp() {
  M5Cardputer.Display.fillScreen(kColorBg);
  drawTopBar("Disabled App", "9/9");
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setTextColor(kColorYellow, kColorBg);
  M5Cardputer.Display.setCursor(2, 28);
  M5Cardputer.Display.println("Selected module disabled");
  M5Cardputer.Display.println("in v0.1.5 safe mode.");
  if (g_disabledMessage.length()) {
    M5Cardputer.Display.println(g_disabledMessage);
  }
  drawSafeFooter("0 BOOT   GO/B BACK");
}

void redrawSafeState() {
  if (g_safeScreen == g_lastSafeScreen && !g_needRedraw) {
    return;
  }

  Serial.print("[SAFE] draw screen begin: ");
  Serial.println(screenName(g_safeScreen));

  switch (g_safeScreen) {
    case SafeModeScreen::BootDiag:
      drawBootDiag();
      break;
    case SafeModeScreen::SafeLauncher:
      drawLauncherScreen();
      break;
    case SafeModeScreen::SafeInputTest:
      drawSafeInputTest();
      break;
    case SafeModeScreen::SafeSystemInfo:
      drawSafeSystemInfo();
      break;
    case SafeModeScreen::SafeRandomizer:
      drawSafeRandomizer();
      break;
    case SafeModeScreen::SafeClock:
      drawSafeClock();
      break;
    case SafeModeScreen::SafeNotesList:
      drawSafeSimpleListScreen("Notes", "/notes", "No .txt notes");
      break;
    case SafeModeScreen::SafeReaderList:
      drawSafeSimpleListScreen("Reader", "/books", "No .txt books");
      break;
    case SafeModeScreen::SafeMusicStatus:
      drawSafeMusicList();
      break;
    case SafeModeScreen::SafeDisabledApp:
      drawSafeDisabledApp();
      break;
  }
  Serial.print("[SAFE] draw screen end: ");
  Serial.println(screenName(g_safeScreen));
  g_needRedraw = false;
  g_lastSafeScreen = g_safeScreen;
  g_lastStateRedrawMs = millis();
}

const char* actionName(SafeAction action) {
  switch (action) {
    case SafeAction::None:
      return "-";
    case SafeAction::GoShort:
      return "GO_SHORT";
    case SafeAction::GoLong:
      return "GO_LONG";
    case SafeAction::Enter:
      return "ENTER";
    case SafeAction::Back:
      return "BACK";
    case SafeAction::Launcher:
      return "LAUNCHER";
    case SafeAction::Next:
      return "NEXT";
    case SafeAction::Prev:
      return "PREV";
    case SafeAction::Up:
      return "UP";
    case SafeAction::Down:
      return "DOWN";
    case SafeAction::RetrySD:
      return "RETRY_SD";
    case SafeAction::Boot:
      return "BOOT";
    case SafeAction::Right:
      return "RIGHT";
    case SafeAction::Text:
      return "TEXT";
  }
  return "-";
}

const char* screenName(SafeModeScreen screen) {
  switch (screen) {
    case SafeModeScreen::BootDiag:
      return "BootDiag";
    case SafeModeScreen::SafeLauncher:
      return "SafeLauncher";
    case SafeModeScreen::SafeInputTest:
      return "SafeInputTest";
    case SafeModeScreen::SafeSystemInfo:
      return "SafeSystemInfo";
    case SafeModeScreen::SafeRandomizer:
      return "SafeRandomizer";
    case SafeModeScreen::SafeClock:
      return "SafeClock";
    case SafeModeScreen::SafeNotesList:
      return "SafeNotesList";
    case SafeModeScreen::SafeReaderList:
      return "SafeReaderList";
    case SafeModeScreen::SafeMusicStatus:
      return "SafeMusicStatus";
    case SafeModeScreen::SafeDisabledApp:
      return "SafeDisabledApp";
  }
  return "Unknown";
}

void formatRawLabelForLog(char* out, size_t outSize, char c) {
  if (!out || outSize == 0) return;
  if (c == '\0') {
    snprintf(out, outSize, "(none)");
    return;
  }

  if (c >= 0x20 && c <= 0x7E) {
    out[0] = c;
    out[1] = '\0';
    return;
  }
  snprintf(out, outSize, "0x%02X", static_cast<uint8_t>(c));
}

void logSafeInput(const char* raw, SafeAction action) {
  Serial.print("[SAFE INPUT] raw=");
  Serial.print(raw ? raw : "(none)");
  Serial.print(" action=");
  Serial.println(actionName(action));
  Serial.flush();
  if (raw && raw[0]) {
    snprintf(g_inputState.lastRaw, sizeof(g_inputState.lastRaw), "%s", raw);
  } else {
    g_inputState.lastRaw[0] = 0;
  }
  g_inputState.lastAction = action;
}

bool testSdManually() {
  Serial.println("[BOOT] GO pressed, testing SD");
  Serial.flush();

  g_sdState.lastError.remove(0);
  g_sdState.mountState = SafeSdMountState::Failed;
  g_sdState.cardSizeBytes = 0;
  g_sdState.cardType = 0;
  g_sdState.testedSpeed = 0;
  M5Cardputer.Display.setTextColor(kColorCyan, kColorBg);
  drawBootDiag();
  M5Cardputer.Display.setCursor(8, M5Cardputer.Display.height() - 26);
  M5Cardputer.Display.println("testing...");

  bool ok = false;
  for (uint8_t i = 0; i < sizeof(kSdSpeeds) / sizeof(kSdSpeeds[0]); ++i) {
    const uint32_t speed = kSdSpeeds[i];
    Serial.printf("[BOOT] SD test speed=%lu\n", speed);
    Serial.flush();

    SD.end();
    SPI.end();
    SPI.begin(kSdSck, kSdMiso, kSdMosi, kSdCs);

    if (!SD.begin(kSdCs, SPI, speed)) {
      Serial.printf("[BOOT] SD test fail at speed=%lu\n", speed);
      Serial.flush();
      continue;
    }

    g_sdState.cardType = SD.cardType();
    g_sdState.cardSizeBytes = SD.cardSize();
    if (g_sdState.cardType == CARD_NONE || g_sdState.cardType == CARD_UNKNOWN || !g_sdState.cardSizeBytes) {
      g_sdState.lastError = String("invalid card type=") + g_sdState.cardType;
      Serial.printf("[BOOT] SD present but invalid type=%u size=%llu\n", g_sdState.cardType, g_sdState.cardSizeBytes);
      Serial.flush();
      continue;
    }

    ok = true;
    g_sdState.mountState = SafeSdMountState::Ok;
    g_sdState.testedSpeed = speed;
    Serial.printf("[BOOT] SD OK speed=%lu card=%u sizeMB=%llu\n", speed, g_sdState.cardType, g_sdState.cardSizeBytes / (1024ULL * 1024ULL));
    Serial.flush();
    break;
  }

  if (!ok) {
    g_sdState.mountState = SafeSdMountState::Failed;
    if (!g_sdState.lastError.length()) {
      g_sdState.lastError = "sd init failed";
    }
  }

  redrawSafeState();
  g_needRedraw = true;
  return ok;
}

SafeAction mapCharToAction(char c) {
  const char lower = static_cast<char>(tolower(static_cast<uint8_t>(c)));

  // Number shortcuts.
  if (c == '1') return SafeAction::Launcher;
  if (c == '0') return SafeAction::Boot;

  // Explicit backspace/text controls.
  if (lower == 'r') return SafeAction::RetrySD;
  if (lower == 'b') return SafeAction::Back;

  // Navigation letters.
  if (lower == 'd' || lower == 'l' || c == '\t') return SafeAction::Next;
  if (lower == 'a' || lower == 'h') return SafeAction::Prev;
  if (lower == 'w' || lower == 'k') return SafeAction::Up;
  if (lower == 's' || lower == 'j') return SafeAction::Down;

  // Punctuation arrows.
  if (c == ';' || c == ':') return SafeAction::Up;
  if (c == ',' || c == '<') return SafeAction::Prev;
  if (c == '.' || c == '>') return SafeAction::Down;
  if (c == '/' || c == '?') return SafeAction::Right;

  // Optional fallback for GO-equivalent key.
  if (lower == 'g') return SafeAction::Enter;

  return SafeAction::Text;
}

SafeAction parseKeyboardAction(char c) {
  if (!c) {
    return SafeAction::None;
  }

  SafeAction action = mapCharToAction(c);

  return action;
}

void setSafeInputStatusForAction(SafeAction action, const char* raw) {
  if (!raw) {
    raw = "(none)";
  }
  g_inputState.enter = false;
  g_inputState.backspace = false;
  g_inputState.enter = action == SafeAction::Enter;
  g_inputState.backspace = action == SafeAction::Back;
  if (action == SafeAction::GoShort) g_inputState.goShort = true;
  if (action == SafeAction::GoLong) g_inputState.goLong = true;
}

void openSelectedLauncherItem() {
  if (g_safeSelectedApp >= kSafeLauncherCount) {
    return;
  }

  const SafeLauncherItem& item = kSafeLauncherItems[g_safeSelectedApp];

  if (item.disabled) {
    g_disabledMessage = item.disabledMessage;
    g_safeScreen = SafeModeScreen::SafeDisabledApp;
    g_needRedraw = true;
    return;
  }

  if (item.requiresSd && g_sdState.mountState != SafeSdMountState::Ok) {
    g_disabledMessage = item.disabledMessage;
    g_safeScreen = SafeModeScreen::SafeDisabledApp;
    g_needRedraw = true;
    return;
  }

  if (item.title[0] == 'S' && item.title[1] == 'D') {
    testSdManually();
    g_safeScreen = SafeModeScreen::BootDiag;
    g_needRedraw = true;
    return;
  }

  g_safeScreen = item.target;
  if (g_safeScreen == SafeModeScreen::SafeRandomizer) {
    // Keep randomizer deterministic until explicit roll.
    g_randomResult = "Maybe";
  }
  if (g_safeScreen == SafeModeScreen::SafeSystemInfo || g_safeScreen == SafeModeScreen::SafeClock) {
    g_lastStateRedrawMs = 0;
  }
  g_needRedraw = true;
}

void handleSafeAction(SafeAction action, const char* raw) {
  if (action == SafeAction::None) {
    return;
  }

  Serial.print("[SAFE] handle action begin: ");
  Serial.println(actionName(action));
  setSafeInputStatusForAction(action, raw);
  logSafeInput(raw, action);

  g_needRedraw = false;

  switch (g_safeScreen) {
    case SafeModeScreen::BootDiag:
      if (action == SafeAction::Launcher) {
        g_safeScreen = SafeModeScreen::SafeLauncher;
        g_needRedraw = true;
      } else if (action == SafeAction::Enter || action == SafeAction::Boot) {
        testSdManually();
      } else if (action == SafeAction::RetrySD) {
        testSdManually();
      }
      break;

    case SafeModeScreen::SafeLauncher:
      if (action == SafeAction::Next) {
        g_safeSelectedApp = (g_safeSelectedApp + 1) % kSafeLauncherCount;
        g_needRedraw = true;
      } else if (action == SafeAction::Prev) {
        g_safeSelectedApp = (g_safeSelectedApp + kSafeLauncherCount - 1) % kSafeLauncherCount;
        g_needRedraw = true;
      } else if (action == SafeAction::Up || action == SafeAction::Down || action == SafeAction::Right) {
        // no-op in single-tile launcher
      } else if (action == SafeAction::Enter) {
        openSelectedLauncherItem();
      } else if (action == SafeAction::Back || action == SafeAction::Boot) {
        g_safeScreen = SafeModeScreen::BootDiag;
        g_needRedraw = true;
      }
      break;

    case SafeModeScreen::SafeInputTest:
      if (action == SafeAction::Back || action == SafeAction::Boot) {
        g_safeScreen = SafeModeScreen::SafeLauncher;
        g_needRedraw = true;
      }
      break;

    case SafeModeScreen::SafeSystemInfo:
      if (action == SafeAction::RetrySD) {
        testSdManually();
      } else if (action == SafeAction::Enter || action == SafeAction::Back || action == SafeAction::Boot) {
        g_safeScreen = SafeModeScreen::SafeLauncher;
        g_needRedraw = true;
      }
      break;

    case SafeModeScreen::SafeRandomizer:
      if (action == SafeAction::Enter) {
        static const char* outcomes[] = {"Yes", "No", "Maybe"};
        g_randomResult = outcomes[random(0, 3)];
        g_needRedraw = true;
      } else if (action == SafeAction::Back || action == SafeAction::Boot) {
        g_safeScreen = SafeModeScreen::SafeLauncher;
        g_needRedraw = true;
      }
      break;

    case SafeModeScreen::SafeClock:
      if (action == SafeAction::Back || action == SafeAction::Enter || action == SafeAction::Boot) {
        g_safeScreen = SafeModeScreen::SafeLauncher;
        g_needRedraw = true;
      }
      break;

    case SafeModeScreen::SafeNotesList:
    case SafeModeScreen::SafeReaderList:
    case SafeModeScreen::SafeMusicStatus:
      if (action == SafeAction::RetrySD) {
        testSdManually();
      } else if (action == SafeAction::Enter || action == SafeAction::Back || action == SafeAction::Boot) {
        g_safeScreen = SafeModeScreen::SafeLauncher;
        g_needRedraw = true;
      }
      break;

    case SafeModeScreen::SafeDisabledApp:
      if (action == SafeAction::Back || action == SafeAction::Boot || action == SafeAction::Enter) {
        g_safeScreen = SafeModeScreen::SafeLauncher;
        g_needRedraw = true;
      }
      break;
  }

  Serial.print("[SAFE] handle action end: ");
  Serial.println(actionName(action));
}

void handleGoShort() {
  handleSafeAction(SafeAction::GoShort, "<go>");

  if (g_safeScreen == SafeModeScreen::BootDiag) {
    testSdManually();
    return;
  }
  if (g_safeScreen == SafeModeScreen::SafeLauncher) {
    openSelectedLauncherItem();
    return;
  }

  // In any other screen, keep GO short as back/confirm behavior.
  if (g_safeScreen == SafeModeScreen::SafeInputTest ||
      g_safeScreen == SafeModeScreen::SafeSystemInfo ||
      g_safeScreen == SafeModeScreen::SafeRandomizer ||
      g_safeScreen == SafeModeScreen::SafeClock ||
      g_safeScreen == SafeModeScreen::SafeNotesList ||
      g_safeScreen == SafeModeScreen::SafeReaderList ||
      g_safeScreen == SafeModeScreen::SafeMusicStatus ||
      g_safeScreen == SafeModeScreen::SafeDisabledApp) {
    g_safeScreen = SafeModeScreen::SafeLauncher;
    g_needRedraw = true;
  }
}

void handleGoLong() {
  handleSafeAction(SafeAction::GoLong, "<go>");
  g_safeScreen = SafeModeScreen::BootDiag;
  g_needRedraw = true;
}

void processButton() {
  const uint32_t now = millis();
  const bool btnDown = M5Cardputer.BtnA.isPressed();
  if (btnDown != g_btnDown && (now - g_lastBtnChangeMs < kBtnDebounceMs)) {
    return;
  }

  if (btnDown != g_btnDown) {
    g_lastBtnChangeMs = now;
    const bool wasDown = g_btnDown;
    if (wasDown && !btnDown) {
      const uint32_t held = now - g_btnDownAtMs;
      if (!g_btnLongHandled && held < kBtnLongPressMs) {
        handleGoShort();
      }
    }
    g_btnDown = btnDown;
    if (btnDown) {
      g_btnDownAtMs = now;
    }
    g_btnLongHandled = false;
    return;
  } else if (btnDown && !g_btnLongHandled && (now - g_btnDownAtMs >= kBtnLongPressMs)) {
    g_btnLongHandled = true;
    handleGoLong();
  }
}

void processKeyboard() {
  const uint32_t now = millis();
  const bool keyChanged = M5Cardputer.Keyboard.isChange();
  if (!keyChanged) {
    g_keyboardConsumedChange = false;
    return;
  }
  if (g_keyboardConsumedChange) {
    return;
  }
  if (now - g_lastKeyboardEventMs < kInputDebounceMs) {
    return;
  }

  const auto keys = M5Cardputer.Keyboard.keysState();
  char rawLabel[16] = {0};
  uint8_t firstRaw = 0;
  char textChar = 0;

  if (keys.enter) {
    snprintf(rawLabel, sizeof(rawLabel), "<enter>");
    handleSafeAction(SafeAction::Enter, rawLabel);
    g_keyboardConsumedChange = true;
    g_lastKeyboardEventMs = now;
    return;
  }
  if (keys.del) {
    snprintf(rawLabel, sizeof(rawLabel), "<bs>");
    handleSafeAction(SafeAction::Back, rawLabel);
    g_keyboardConsumedChange = true;
    g_lastKeyboardEventMs = now;
    return;
  }

  for (uint8_t idx = 0; idx < sizeof(keys.word); ++idx) {
    const uint8_t c = static_cast<uint8_t>(keys.word[idx]);
    if (c == 0) {
      continue;
    }
    if (!firstRaw) {
      firstRaw = c;
    }
    if ((c >= 0x20) && (c <= 0x7E)) {
      textChar = static_cast<char>(c);
      break;
    }
  }

  if (firstRaw == '\t') {
    snprintf(rawLabel, sizeof(rawLabel), "<tab>");
    handleSafeAction(SafeAction::Next, rawLabel);
    g_keyboardConsumedChange = true;
    g_lastKeyboardEventMs = now;
    return;
  }

  if (!firstRaw) {
    return;
  }

  if (!textChar) {
    formatRawLabelForLog(rawLabel, sizeof(rawLabel), static_cast<char>(firstRaw));
    handleSafeAction(SafeAction::Text, rawLabel);
    g_keyboardConsumedChange = true;
    g_lastKeyboardEventMs = now;
    return;
  }

  if (!rawLabel[0]) {
    formatRawLabelForLog(rawLabel, sizeof(rawLabel), textChar);
  }

  handleSafeAction(parseKeyboardAction(textChar), rawLabel);
  g_keyboardConsumedChange = true;
  g_lastKeyboardEventMs = now;
}
}  // namespace
#endif

#if !FEATURE_ULTRA_SAFE_BOOT
InputManager input;
TerminalUI ui;
StorageManager storage;
SettingsManager settings;
PowerManager power;
NetworkManager network;
AppManager apps;

AppContext context;

#if FEATURE_MUSIC
MusicApp musicApp;
#endif
#if FEATURE_RECORDER
RecorderApp recorderApp;
#endif
#if FEATURE_NOTES
NotesApp notesApp;
#endif
#if FEATURE_READER
ReaderApp readerApp;
#endif
#if FEATURE_CLOCK
ClockApp clockApp;
#endif
#if FEATURE_NETWORK
NetworkApp networkApp;
#endif
#if FEATURE_WEB_FILE_MANAGER
WebFileManagerApp webFileManagerApp;
#endif
#if FEATURE_RANDOMIZER
RandomizerApp randomizerApp;
#endif
#if FEATURE_BROWSER
BrowserApp browserApp;
#endif
#if FEATURE_AI_TEXT
AIApp aiApp;
#endif
#if FEATURE_PAYMENTS_INFO
PaymentsApp paymentsApp;
#endif
#if FEATURE_INPUT_DIAGNOSTICS
InputDiagnosticsApp inputDiagnosticsApp;
#endif
#if FEATURE_SYSTEM_INFO
SystemInfoApp systemInfoApp;
#endif

static void registerApps() {
  apps.begin(context);
#if FEATURE_MUSIC
  apps.add(&musicApp);
#endif
#if FEATURE_RECORDER
  apps.add(&recorderApp);
#endif
#if FEATURE_NOTES
  apps.add(&notesApp);
#endif
#if FEATURE_READER
  apps.add(&readerApp);
#endif
#if FEATURE_CLOCK
  apps.add(&clockApp);
#endif
#if FEATURE_NETWORK
  apps.add(&networkApp);
#endif
#if FEATURE_WEB_FILE_MANAGER
  apps.add(&webFileManagerApp);
#endif
#if FEATURE_RANDOMIZER
  apps.add(&randomizerApp);
#endif
#if FEATURE_BROWSER
  apps.add(&browserApp);
#endif
#if FEATURE_AI_TEXT
  apps.add(&aiApp);
#endif
#if FEATURE_PAYMENTS_INFO
  apps.add(&paymentsApp);
#endif
#if FEATURE_INPUT_DIAGNOSTICS
  apps.add(&inputDiagnosticsApp);
#endif
#if FEATURE_SYSTEM_INFO
  apps.add(&systemInfoApp);
#endif
}
#endif

void setup() {
#if FEATURE_ULTRA_SAFE_BOOT
  Serial.begin(115200);
  Serial.println("[SAFE] settings save skipped in ultra-safe mode");
  delay(1000);
  Serial.println();
  Serial.println("[BOOT] 000 setup entered v0.1.5-safe-input ultra-safe");
  Serial.flush();
  Serial.println("[BOOT] 010 before M5Cardputer.begin");
  Serial.flush();

  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  Serial.println("[BOOT] 020 M5Cardputer.begin done");
  Serial.flush();

  g_btnDown = M5Cardputer.BtnA.isPressed();
  g_btnDownAtMs = g_btnDown ? millis() : 0;
  g_btnLongHandled = false;
  g_needRedraw = true;
  g_safeScreen = SafeModeScreen::BootDiag;
  g_safeSelectedApp = 0;

  Serial.println("[BOOT] 030 safe boot screen rendered");
  Serial.flush();

  g_lastHeartbeatMs = millis();
  g_lastStateRedrawMs = 0;
  redrawSafeState();
#else
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  Serial.begin(115200);

  ui.begin();
  ui.header("Boot");
  ui.line(2, "Init...");
  ui.line(4, "SD and modules");
  ui.pushFrame();

  input.begin();
  input.setDisplayAwake(true);
  const bool cardMounted = storage.begin();
  settings.begin(storage);
  network.begin(storage);
  power.begin(input, settings);
  context.ui = &ui;
  context.storage = &storage;
  context.settings = &settings;
  context.power = &power;
  context.network = &network;
  context.input = &input;
  registerApps();

  if (!cardMounted) {
    ui.clearFrame();
    ui.header("Storage");
    ui.line(2, "Insert SD card", TerminalUI::Yellow);
    ui.line(3, "GO retry / long GO menu", TerminalUI::Yellow);
    ui.pushFrame();
  }
#endif
}

void loop() {
#if FEATURE_ULTRA_SAFE_BOOT
  M5Cardputer.update();
  processButton();
  processKeyboard();

  const uint32_t now = millis();
  if (now - g_lastHeartbeatMs >= kBootHeartbeatMs) {
    ++g_inputState.heartbeat;
    Serial.printf("[BOOT] loop alive ms=%lu heap=%u\n", now, ESP.getFreeHeap());
    Serial.flush();
    g_lastHeartbeatMs = now;
  }

  if ((g_safeScreen == SafeModeScreen::SafeSystemInfo || g_safeScreen == SafeModeScreen::SafeClock) &&
      now - g_lastStateRedrawMs >= kStateHeartbeatMs) {
    g_needRedraw = true;
  }

  if (g_safeScreen == SafeModeScreen::SafeClock) {
    if (g_needRedraw) {
      redrawSafeState();
    }
  } else if (g_needRedraw) {
    redrawSafeState();
  }

  delay(10);
#else
  input.update();
  network.update();

  InputEvent event;
  while (input.pollEvent(event)) {
    if (event.action == InputAction::Wake) {
      Serial.println("[Power] wake requested");
      power.wakeDisplay();
      apps.onInput(event);
      continue;
    }
    power.notifyUserActivity();
    apps.onInput(event);
  }

  apps.update();
  power.update(apps.backgroundBusy());

  if (power.displayAwake()) apps.draw();
  delay(5);
#endif
}
