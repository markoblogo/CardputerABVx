#include "TerminalUI.h"

#include <M5Cardputer.h>

void TerminalUI::begin() {
  M5Cardputer.Display.setRotation(1);
  frame_.setColorDepth(16);
  frame_.createSprite(M5Cardputer.Display.width(), M5Cardputer.Display.height());
  ready_ = true;
  frame_.setTextSize(1);
  frame_.setTextFont(1);
  frame_.setTextDatum(TL_DATUM);
  charW_ = 6;
  charH_ = 8;
  rows_ = M5Cardputer.Display.height() / charH_;
  cols_ = M5Cardputer.Display.width() / charW_;
  clearFrame();
}

void TerminalUI::clear() {
  clearFrame();
  pushFrame();
}

void TerminalUI::clearFrame() {
  if (!ready_) {
    M5Cardputer.Display.fillScreen(Black);
    return;
  }
  frame_.fillScreen(Black);
}

void TerminalUI::pushFrame() {
  if (!ready_) {
    return;
  }
  frame_.pushSprite(0, 0);
}

void TerminalUI::drawTopBar(const char* title, const char* status) {
  if (!ready_) return;
  frame_.fillRect(0, 0, M5Cardputer.Display.width(), charH_ + 6, Black);
  String left = title ? String(title) : "";
  String right = status ? String(status) : "";
  frame_.setTextSize(1);
  clippedText(0, 0, left, Green, Black);
  if (right.length()) {
    int xr = M5Cardputer.Display.width() - right.length() * charW_;
    if (xr < 0) xr = 0;
    clippedText(xr, 0, right, Yellow, Black);
  }
  frame_.drawFastHLine(0, charH_ + 3, M5Cardputer.Display.width(), Green);
}

void TerminalUI::header(const char* title) {
  drawTopBar(title, nullptr);
}

void TerminalUI::footer(const char* help) {
  drawFooter(help);
}

void TerminalUI::drawFooter(const char* help) {
  if (!ready_) return;
  int y = footerRow() * charH_;
  String text = help ? String(help) : "";
  frame_.fillRect(0, y, M5Cardputer.Display.width(), charH_ + 2, Black);
  frame_.drawFastHLine(0, y, M5Cardputer.Display.width(), Dim);
  frame_.setTextSize(1);
  frame_.setTextColor(Yellow, Black);
  int maxChars = M5Cardputer.Display.width() / charW_;
  if (text.length() > static_cast<size_t>(maxChars)) {
    text = text.substring(0, maxChars);
  }
  frame_.setCursor(0, y + 1);
  frame_.print(text);
}

void TerminalUI::line(uint8_t row, const String& text, uint16_t color) {
  if (!ready_) return;
  if (row >= rows_) return;
  int y = row * charH_;
  frame_.fillRect(0, y, M5Cardputer.Display.width(), charH_, Black);
  clippedText(0, y, text, color);
}

void TerminalUI::status(const String& text, uint16_t color) {
  if (!ready_) return;
  if (rows_ > 1) {
    line(footerRow() - 1, text, color);
  }
}

void TerminalUI::listItem(uint8_t row, const String& text, bool selected) {
  if (!ready_) return;
  if (row >= rows_) return;
  int y = row * charH_;
  uint16_t bg = selected ? Green : Black;
  uint16_t fg = selected ? Black : White;
  frame_.fillRect(0, y, M5Cardputer.Display.width(), charH_, bg);
  clippedText(0, y, String(selected ? "> " : "  ") + text, fg, bg);
}

void TerminalUI::drawCenteredTitle(const String& title) {
  if (!ready_) return;
  int x = (M5Cardputer.Display.width() - min<int>(title.length() * 12, M5Cardputer.Display.width())) / 2;
  if (x < 0) x = 0;
  frame_.setTextSize(2);
  frame_.setTextColor(White, Black);
  frame_.setCursor(x, 52);
  frame_.print(title.substring(0, 20));
  frame_.setTextSize(1);
}

void TerminalUI::drawLargeIcon(const char* icon) {
  if (!ready_) return;
  frame_.setTextSize(2);
  frame_.setTextColor(White, Black);
  String marker = icon ? String(icon) : "[*]";
  if (marker.length() > 7) marker = marker.substring(0, 7);
  int x = (M5Cardputer.Display.width() - marker.length() * 12) / 2;
  if (x < 0) x = 0;
  frame_.setCursor(x, 28);
  frame_.print(marker);
  frame_.setTextSize(1);
}

void TerminalUI::drawTile(const String& title, const char* icon, uint8_t index, uint8_t total) {
  if (!ready_) return;
  drawLargeIcon(icon);
  drawCenteredTitle(title);
  line(10, String(index) + "/" + String(total), TerminalUI::Green);
  frame_.setTextSize(1);
  frame_.setTextColor(White, Black);
}

void TerminalUI::drawProgressBar(uint8_t x, uint8_t y, uint16_t width, uint8_t value, uint8_t maxValue) {
  if (!ready_) return;
  if (maxValue == 0) return;
  if (width > M5Cardputer.Display.width() - x) width = M5Cardputer.Display.width() - x;
  uint16_t filled = static_cast<uint16_t>(width * value / maxValue);
  frame_.drawRect(x, y, width, 6, White);
  frame_.fillRect(x + 1, y + 1, filled ? filled - 1 : 0, 4, Green);
}

void TerminalUI::drawValueBar(uint8_t x, uint8_t y, uint16_t width, uint16_t value, uint16_t maxValue) {
  if (!ready_) return;
  if (maxValue == 0) return;
  if (width > M5Cardputer.Display.width() - x) width = M5Cardputer.Display.width() - x;
  uint16_t filled = static_cast<uint16_t>(width * value / maxValue);
  frame_.drawRect(x, y, width, 6, White);
  frame_.fillRect(x + 1, y + 1, filled ? filled - 1 : 0, 4, Yellow);
}

void TerminalUI::clippedText(int x, int y, const String& text, uint16_t color, uint16_t bg) {
  if (!ready_) return;
  String clipped = text;
  int availableCols = M5Cardputer.Display.width() / charW_;
  if (clipped.length() > static_cast<size_t>(availableCols)) {
    clipped = clipped.substring(0, availableCols);
  }
  frame_.setTextColor(color, bg);
  frame_.setCursor(x, y);
  frame_.print(clipped);
}
