#include "AppManager.h"
#include "StorageManager.h"
#include "SettingsManager.h"
#include "Features.h"
#include "NetworkManager.h"

#include "TerminalUI.h"
#include "InputManager.h"

static bool isPressOrRepeat(const InputEvent& event) {
  return event.type == InputEventType::Press || event.type == InputEventType::Repeat;
}

void AppManager::begin(AppContext& context) {
  ctx_ = &context;
  safeMode_ = FEATURE_SAFE_BOOT != 0;
}

void AppManager::add(App* app) {
  if (count_ >= MaxApps) return;
  apps_[count_++] = app;
  app->begin(*ctx_);
}

void AppManager::update() {
  if (active_ < 0) return;
  if (active_ >= count_ || !apps_[active_]) {
    Serial.println("[AppManager] invalid active app, returning to launcher");
    active_ = -1;
    selected_ = 0;
    return;
  }
  apps_[active_]->update();
}

void AppManager::draw() {
  if (!ctx_ || !ctx_->ui) return;
  const uint32_t now = millis();
  if (active_ >= 0) {
    if (active_ >= count_ || !apps_[active_]) {
      Serial.println("[AppManager] invalid app, returning to launcher");
      active_ = -1;
      selected_ = 0;
      requestRedraw();
    } else {
      if (!dirty_ && !apps_[active_]->wantsBackgroundWork() && (now - lastDrawMs_ < 150)) return;
    }
  } else {
    if (!dirty_ && (now - lastDrawMs_ < 120)) return;
  }

  ctx_->ui->clearFrame();

  if (active_ >= 0) {
    if (!apps_[active_]) return;
    apps_[active_]->draw();
    ctx_->ui->footer(apps_[active_]->getHelpLine());
    if (ctx_->input) ctx_->input->setInputContext(apps_[active_]->inputContext());
  } else {
    if (safeMode_ && ctx_->storage && !ctx_->storage->isMounted()) {
      drawSdError();
      return;
    }
    drawMenu();
    if (ctx_->input) ctx_->input->setInputContext(InputContext::Menu);
  }
  ctx_->ui->pushFrame();
  dirty_ = false;
  lastDrawMs_ = now;
}

void AppManager::onInput(const InputEvent& event) {
  if (!ctx_) return;

  if (safeMode_ && ctx_->storage && !ctx_->storage->isMounted() && active_ < 0) {
    if (event.action == InputAction::Select || event.action == InputAction::Enter) {
      Serial.println("[AppManager] manual SD retry requested from launcher");
      ctx_->storage->retryMount();
      requestRedraw();
      return;
    }
  }

  if (active_ >= 0 && ctx_->input) {
    if (active_ < count_ && apps_[active_]) ctx_->input->setInputContext(apps_[active_]->inputContext());
  }
  if (event.action == InputAction::Wake) {
    // Wake suppression is handled in PowerManager and not forwarded into app handlers.
    dirty_ = true;
    return;
  }
  if (active_ >= 0) {
    if (active_ >= count_ || !apps_[active_]) {
      active_ = -1;
      dirty_ = true;
      if (ctx_->input) ctx_->input->setInputContext(InputContext::Menu);
      return;
    }
    if (event.action == InputAction::Back && event.type == InputEventType::LongPress) {
      active_ = -1;
      dirty_ = true;
      if (ctx_->input) ctx_->input->setInputContext(InputContext::Menu);
      return;
    }
    apps_[active_]->onInput(event);
    dirty_ = true;
    return;
  }
  if (!isPressOrRepeat(event)) return;
  if (event.action == InputAction::Up && selected_ > 0) --selected_;
  else if (event.action == InputAction::Down && selected_ < static_cast<int8_t>(count_) - 1) ++selected_;
  else if (event.action == InputAction::Left && count_) selected_ = (selected_ + count_ - 1) % count_;
  else if (event.action == InputAction::Right && count_) selected_ = (selected_ + 1) % count_;
  else if (event.action == InputAction::TextChar && event.text >= '0' && event.text <= '9') {
    uint8_t index = event.text - '0';
    if (index == 0) index = 10;
    if (index > 0 && index <= count_) active_ = index - 1;
  }
  else if (event.action == InputAction::Select || event.action == InputAction::Enter) active_ = selected_;
  dirty_ = true;
}

bool AppManager::backgroundBusy() const {
  if (!count_) return false;
  for (uint8_t i = 0; i < count_; ++i) {
    if (!apps_[i]) continue;
    if (apps_[i]->wantsBackgroundWork()) return true;
  }
  return false;
}

App* AppManager::current() const {
  return active_ >= 0 ? apps_[active_] : nullptr;
}

void AppManager::requestRedraw() {
  dirty_ = true;
}

static const char* appIcon(uint8_t index) {
  static const char* icons[] = {
    "MUS",
    "REC",
    "NOTE",
    "BOOK",
    "CLOCK",
    "NET",
    "FILE",
    "RAND",
    "BROW",
    "AI",
    "PAY",
    "DIAG",
    "CAST",
    "INFO"
  };
  return icons[index % 14];
}

void AppManager::drawMenu() {
  if (!count_) return;
  uint8_t safe = selected_;
  if (safe >= count_) safe = 0;
  ctx_->ui->drawTopBar("CARDPUTER ABVx", String(String(safe + 1) + "/" + String(count_)).c_str());
  ctx_->ui->drawTile(
    String(apps_[safe]->getTitle()),
    appIcon(safe),
    safe + 1,
    count_);
  String castHost = ctx_->settings ? ctx_->settings->get().castHost : String("192.168.4.1");
  uint16_t castPort = ctx_->settings ? ctx_->settings->get().castPort : 3000;
  if (castHost.length() == 0) castHost = "192.168.4.1";
  if (castPort == 0) castPort = 3000;
  String castState = String(ctx_->network && ctx_->network->connected() ? "CAST ON " : "CAST OFF");
  castState += castHost + ":" + String(castPort);
  ctx_->ui->status(
    "UP/DN/< >/TAB:APP  1-0 open  " + castState,
    TerminalUI::Dim
  );
  ctx_->ui->footer("GO OPEN  HOLD GO:BACK");
}

void AppManager::drawSdError() {
  ctx_->ui->clearFrame();
  ctx_->ui->header("SD NOT MOUNTED");
  ctx_->ui->line(2, "Check card");
  ctx_->ui->line(3, "GO Retry");
  ctx_->ui->line(4, "Hold GO Menu");
  ctx_->ui->line(5, String("Last error: ") + (ctx_->storage ? ctx_->storage->getLastError() : "unknown"));
  ctx_->ui->footer("GO Retry  HOLD GO:Menu");
  ctx_->ui->pushFrame();
  dirty_ = false;
  lastDrawMs_ = millis();
}
