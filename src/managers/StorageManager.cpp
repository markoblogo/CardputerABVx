#include "StorageManager.h"

#include <SD.h>
#include <SPI.h>

#include <Arduino.h>

namespace {
constexpr uint8_t kSdSck = 40;
constexpr uint8_t kSdMiso = 39;
constexpr uint8_t kSdMosi = 14;
constexpr uint8_t kSdCs = 12;
const uint32_t kRetrySpeeds[] = {25000000UL, 1000000UL, 4000000UL, 10000000UL, 25000000UL};
}

static const char* cardTypeName(uint8_t type) {
  switch (type) {
    case CARD_NONE: return "NONE";
    case CARD_MMC: return "MMC";
    case CARD_SD: return "SD";
    case CARD_SDHC: return "SDHC";
    case CARD_UNKNOWN: return "UNKNOWN";
    default: return "UNSUPPORTED";
  }
}

bool StorageManager::beginWithSpeed(uint32_t speed) {
  return SD.begin(kSdCs, SPI, speed);
}

bool StorageManager::retryMount() {
  Serial.println("[Storage] SD init begin");
  Serial.printf("[Storage] SPI pins SCK=%lu MISO=%lu MOSI=%lu CS=%lu\n",
                kSdSck, kSdMiso, kSdMosi, kSdCs);
  SD.end();
  SPI.end();
  SPI.begin(kSdSck, kSdMiso, kSdMosi, kSdCs);
  ready_ = false;
  cardType_ = CARD_NONE;
  cardSizeBytes_ = 0;
  lastError_.remove(0);
  lastInitAttemptMs_ = millis();

  for (uint8_t i = 0; i < sizeof(kRetrySpeeds) / sizeof(kRetrySpeeds[0]); ++i) {
    const uint32_t speed = kRetrySpeeds[i];
    Serial.printf("[Storage] trying SD.begin speed=%lu\n", speed);
    SD.end();
    if (!beginWithSpeed(speed)) {
      Serial.println("[Storage] SD init failed");
      lastError_ = String("SD.begin failed at speed=\"") + speed + "\"";
      continue;
    }

    cardType_ = SD.cardType();
    cardSizeBytes_ = SD.cardSize();
    if (cardType_ == CARD_NONE) {
      Serial.printf("[Storage] cardType=%s\n", cardTypeName(cardType_));
      Serial.printf("[Storage] cardSizeMB=%llu\n", cardSizeBytes_ / (1024ULL * 1024ULL));
      lastError_ = "No card in slot";
      Serial.println("[Storage] SD init failed");
      continue;
    }
    if (cardType_ == CARD_UNKNOWN) {
      Serial.printf("[Storage] cardType=%s\n", cardTypeName(cardType_));
      Serial.printf("[Storage] cardSizeMB=%llu\n", cardSizeBytes_ / (1024ULL * 1024ULL));
      lastError_ = "Unknown card type";
      Serial.println("[Storage] SD init failed");
      continue;
    }
    if (!cardSizeBytes_) {
      lastError_ = "Card present but size is zero";
      Serial.println("[Storage] SD init failed");
      continue;
    }

    Serial.println("[Storage] SD init OK");
    Serial.printf("[Storage] cardType=%s\n", cardTypeName(cardType_));
    Serial.printf("[Storage] cardSizeMB=%llu\n", cardSizeBytes_ / (1024ULL * 1024ULL));
    Serial.printf("[Storage] totalBytes=%llu\n", cardSizeBytes_);
    Serial.printf("[Storage] usedBytes=%llu\n", SD.usedBytes());
    ready_ = true;
    lastError_.remove(0);
    ensureFolders();
    return true;
  }

  lastError_ = "SD mount failed";
  return false;
}

void StorageManager::logStatus() {
  Serial.printf("[Storage] ready=%s error=%s cardType=%s cardSizeMB=%llu totalBytes=%llu lastInitAttemptMs=%lu\n",
                ready_ ? "true" : "false",
                lastError_.length() ? lastError_.c_str() : "none",
                cardTypeName(cardType_),
                cardSizeBytes_ / (1024ULL * 1024ULL),
                cardSizeBytes_,
                lastInitAttemptMs_);
}

bool StorageManager::ensureMounted() {
  if (ready_) return true;
  return retryMount();
}

bool StorageManager::begin() {
  return retryMount();
}

void StorageManager::ensureFolders() {
  if (!ready_) return;
  const char* dirs[] = {"/music", "/recordings", "/notes", "/books", "/browser",
                        "/browser/bookmarks", "/browser/saved_pages", "/ai",
                        "/config", "/logs", "/tmp"};
  for (auto dir : dirs) {
    if (!exists(dir)) makeDir(dir);
  }
}

bool StorageManager::makeDir(const char* path) {
  if (!ready_ || !path || !path[0]) return false;
  if (exists(path)) return true;
  return SD.mkdir(path);
}

bool StorageManager::erase(const char* path) {
  if (!ready_ || !path || !path[0]) return false;
  if (!exists(path)) return true;
  return SD.remove(path);
}

bool StorageManager::rmdir(const char* path) {
  if (!ready_ || !path || !path[0]) return false;
  if (!exists(path)) return true;
  return SD.rmdir(path);
}

File StorageManager::open(const char* path, const char* mode) {
  if (!ready_) return File();
  return SD.open(path, mode);
}

bool StorageManager::exists(const char* path) const {
  return ready_ && SD.exists(path);
}

bool StorageManager::isDir(const char* path) const {
  if (!ready_ || !path || !path[0]) return false;
  File target = SD.open(path);
  if (!target) return false;
  bool result = target.isDirectory();
  target.close();
  return result;
}

bool StorageManager::writeText(const char* path, const String& value) {
  if (!ready_ || !path || !path[0]) return false;
  if (exists(path)) {
    if (!erase(path)) return false;
  }
  File f = open(path, FILE_WRITE);
  if (!f) return false;
  f.print(value);
  f.close();
  return true;
}

String StorageManager::readText(const char* path, size_t maxBytes) {
  File f = open(path, FILE_READ);
  if (!f) return "";
  String out;
  while (f.available() && out.length() < maxBytes) out += static_cast<char>(f.read());
  f.close();
  return out;
}

void StorageManager::log(const String& message) {
  if (!ready_) return;
  File f = SD.open("/logs/system.log", FILE_APPEND);
  if (!f) return;
  f.printf("[%lu] %s\n", millis(), message.c_str());
  f.close();
}

uint16_t StorageManager::listFiles(const char* dir, const char* ext, String* out, uint16_t maxItems) {
  if (!ready_) return 0;
  File root = SD.open(dir);
  if (!root || !root.isDirectory()) return 0;
  uint16_t count = 0;
  for (File file = root.openNextFile(); file && count < maxItems; file = root.openNextFile()) {
    String name = file.name();
    if (file.isDirectory()) continue;
    String lower = name;
    String lowerExt = ext ? String(ext) : "";
    if (lowerExt.length()) lowerExt.toLowerCase();
    lower.toLowerCase();
    bool allowedExt = !ext || lower.endsWith(lowerExt);
    bool hidden = name.startsWith(".") || name.startsWith("._") || name.equals(".DS_Store");
    if (!hidden && allowedExt) out[count++] = name;
  }
  root.close();
  return count;
}

String StorageManager::nextNumberedPath(const char* dir, const char* prefix, const char* ext) {
  for (uint16_t i = 1; i < 10000; ++i) {
    String path = String(dir) + "/" + prefix + String(i) + ext;
    if (!exists(path.c_str())) return path;
  }
  return String(dir) + "/" + prefix + "9999" + ext;
}
