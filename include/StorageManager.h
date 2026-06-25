#pragma once

#include <Arduino.h>
#include <FS.h>

class StorageManager {
public:
  bool begin();
  bool ready() const { return ready_; }
  bool isMounted() const { return ready_; }
  const char* getLastError() const { return lastError_.c_str(); }
  uint32_t lastInitAttemptMs() const { return lastInitAttemptMs_; }
  uint8_t cardType() const { return cardType_; }
  uint64_t cardSizeBytes() const { return cardSizeBytes_; }
  bool retryMount();
  void logStatus();
  bool ensureMounted();
  void ensureFolders();
  bool makeDir(const char* path);
  bool erase(const char* path);
  bool rmdir(const char* path);
  File open(const char* path, const char* mode);
  bool exists(const char* path) const;
  bool isDir(const char* path) const;
  bool writeText(const char* path, const String& value);
  String readText(const char* path, size_t maxBytes = 8192);
  void log(const String& message);
  uint16_t listFiles(const char* dir, const char* ext, String* out, uint16_t maxItems);
  String nextNumberedPath(const char* dir, const char* prefix, const char* ext);

private:
  bool ready_ = false;
  String lastError_;
  uint32_t lastInitAttemptMs_ = 0;
  uint8_t cardType_ = 0;
  uint64_t cardSizeBytes_ = 0;
  bool beginWithSpeed(uint32_t speed);
};
