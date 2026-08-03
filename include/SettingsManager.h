#pragma once

#include <Arduino.h>

class StorageManager;

struct Settings {
  uint16_t screenTimeoutSec = 0;
  uint8_t brightness = 160;
  uint8_t volume = 7;
  bool shuffle = false;
  int timezoneOffsetMin = 60;
  String webPin = "";
  String castHost = "192.168.4.1";
  uint16_t castPort = 3000;
  bool castDebug = false;
};

class SettingsManager {
public:
  bool begin(StorageManager& storage);
  const Settings& get() const { return settings_; }
  Settings& edit() { return settings_; }
  void save();

private:
  StorageManager* storage_ = nullptr;
  Settings settings_;
};
