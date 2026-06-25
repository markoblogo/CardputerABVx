#pragma once

#include <M5GFX.h>
#include <Arduino.h>

class TerminalUI {
public:
  static constexpr uint16_t Black = 0x0000;
  static constexpr uint16_t White = 0xFFFF;
  static constexpr uint16_t Green = 0x07E0;
  static constexpr uint16_t Yellow = 0xFFE0;
  static constexpr uint16_t Red = 0xF800;
  static constexpr uint16_t Dim = 0x7BEF;

  void begin();
  void clear();
  void clearFrame();
  void pushFrame();
  void header(const char* title);
  void footer(const char* help);
  void line(uint8_t row, const String& text, uint16_t color = White);
  void status(const String& text, uint16_t color = Yellow);
  void listItem(uint8_t row, const String& text, bool selected);
  void drawTopBar(const char* title, const char* status = nullptr);
  void drawFooter(const char* help);
  void drawCenteredTitle(const String& title);
  void drawLargeIcon(const char* icon);
  void drawTile(const String& title, const char* icon, uint8_t index, uint8_t total);
  void drawProgressBar(uint8_t x, uint8_t y, uint16_t width, uint8_t value, uint8_t maxValue);
  void drawValueBar(uint8_t x, uint8_t y, uint16_t width, uint16_t value, uint16_t maxValue);
  int rows() const { return rows_; }
  int cols() const { return cols_; }
  int footerRow() const { return rows_ - 1; }

private:
  int charW_ = 6;
  int charH_ = 8;
  int rows_ = 17;
  int cols_ = 40;
  bool ready_ = false;
  M5Canvas frame_;
  void clippedText(int x, int y, const String& text, uint16_t color, uint16_t bg = Black);
};
