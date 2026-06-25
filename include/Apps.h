#pragma once

#include "App.h"
#include "FileBrowser.h"
#include "TextEditor.h"
#include <FS.h>
#include <WebServer.h>

class AudioGeneratorMP3;
class AudioFileSourceSD;
class AudioFileSourceID3;
class AudioOutputM5Speaker;

class MusicApp : public App {
public:
  void begin(AppContext& context) override;
  void update() override;
  void draw() override;
  void onInput(const InputEvent& event) override;
  const char* getTitle() const override { return "Music"; }
  const char* getHelpLine() const override { return "GO PLAY/PAUSE  > NEXT  UP/DN VOL  HOLD GO:BACK"; }
  bool wantsBackgroundWork() const override { return playing_; }
private:
  enum class State { NoSD, DirMissing, Empty, Ready };
  FileBrowser files_;
  State state_ = State::NoSD;
  bool playing_ = false;
  bool shuffle_ = false;
  uint8_t volume_ = 7;
  uint32_t vizTick_ = 0;
  uint8_t viz_ = 0;
  String status_ = "idle";
  AudioGeneratorMP3* mp3_ = nullptr;
  AudioFileSourceSD* file_ = nullptr;
  AudioFileSourceID3* id3_ = nullptr;
  AudioOutputM5Speaker* output_ = nullptr;
  bool startTrack();
  void stopTrack();
  void nextTrack();
  void applyVolume();
  void refreshLibrary();
};

class RecorderApp : public App {
public:
  void begin(AppContext& context) override;
  void update() override;
  void draw() override;
  void onInput(const InputEvent& event) override;
  const char* getTitle() const override { return "Recorder"; }
  const char* getHelpLine() const override { return "GO:REC/PAUSE  ENT:SAVE  HOLD GO:MENU"; }
  bool wantsBackgroundWork() const override { return recording_; }
private:
  bool recording_ = false;
  bool paused_ = false;
  uint32_t startedAt_ = 0;
  uint32_t elapsedMs_ = 0;
  uint32_t dataBytes_ = 0;
  uint16_t recordingCount_ = 0;
  String path_;
  File file_;
  static constexpr uint32_t SampleRate = 16000;
  static constexpr size_t ChunkSamples = 512;
  int16_t chunk_[ChunkSamples];
  void startRecording();
  void stopRecording();
  void writeWavHeader(uint32_t dataBytes);
  String makeRecordingPath();
};

class NotesApp : public App {
public:
  void begin(AppContext& context) override;
  void update() override;
  void draw() override;
  void onInput(const InputEvent& event) override;
  InputContext inputContext() const override { return editing_ ? InputContext::TextEntry : InputContext::Navigation; }
  const char* getTitle() const override { return "Notes"; }
  const char* getHelpLine() const override { return editing_ ? "GO:SAVE  ENT:NEWLINE  BSP:DEL  HOLD GO:BACK" : "GO/ENT:OPEN  N:NEW  UP/DN:MOVE"; }
private:
  FileBrowser files_;
  TextEditor editor_;
  bool editing_ = false;
  String path_;
  uint32_t lastAutosave_ = 0;
  void openSelected();
  void save();
  void newNote();
};

class ReaderApp : public App {
public:
  void begin(AppContext& context) override;
  void draw() override;
  void onInput(const InputEvent& event) override;
  const char* getTitle() const override { return "Reader"; }
  const char* getHelpLine() const override { return speedMode_ ? "GO:PAUSE  <-/->:WPM  UP/DN:CHUNK  HOLD GO:MENU" : "UP/DN:LINE  <-/->:PAGE  ENT:MODE"; }
private:
  FileBrowser files_;
  bool opened_ = false;
  bool speedMode_ = false;
  bool paused_ = true;
  String book_;
  String text_;
  uint16_t pos_ = 0;
  uint16_t wpm_ = 300;
};

class ClockApp : public App {
public:
  void begin(AppContext& context) override;
  void update() override;
  void draw() override;
  void onInput(const InputEvent& event) override;
  const char* getTitle() const override { return "Clock"; }
  const char* getHelpLine() const override { return "GO:START  ENT:RESET/SET  <-/->:MODE  HOLD GO:MENU"; }
  bool wantsBackgroundWork() const override { return running_; }
private:
  uint8_t mode_ = 0;
  bool running_ = false;
  uint32_t baseMs_ = 0;
  uint32_t elapsedMs_ = 0;
  uint32_t timerMs_ = 5 * 60 * 1000UL;
  bool timerDone_ = false;
  bool ntpStarted_ = false;
  bool timeKnown_ = false;
  int manualOffsetMin_ = 0;
  void syncNtp();
};

class NetworkApp : public App {
public:
  void begin(AppContext& context) override;
  void draw() override;
  void onInput(const InputEvent& event) override;
  InputContext inputContext() const override { return password_ ? InputContext::TextEntry : InputContext::Navigation; }
  const char* getTitle() const override { return "Network"; }
  const char* getHelpLine() const override { return password_ ? "TYPE PASS  ENT:CONNECT  HOLD GO:BACK" : "GO:SCAN  ENT:PASS  UP/DN:SSID"; }
private:
  int scanCount_ = 0;
  int selected_ = 0;
  bool password_ = false;
  String ssid_;
  TextEditor pass_;
  String status_ = "idle";
  uint32_t connectStarted_ = 0;
};

class WebFileManagerApp : public App {
public:
  void begin(AppContext& context) override;
  void update() override;
  void draw() override;
  void onInput(const InputEvent& event) override;
  const char* getTitle() const override { return "Web File Manager"; }
  const char* getHelpLine() const override { return "GO:START/STOP  HOLD GO:MENU"; }
  bool wantsBackgroundWork() const override { return running_; }
private:
  WebServer server_{80};
  bool running_ = false;
  File uploadFile_;
  String uploadPath_;
  void start();
  void stop();
  String cleanPath(const String& raw, bool* ok = nullptr) const;
  void handleRoot();
  void handleList();
  void handleDownload();
  void handleUpload();
  void handleMkdir();
  void handleDelete();
};

class RandomizerApp : public App {
public:
  void draw() override;
  void onInput(const InputEvent& event) override;
  const char* getTitle() const override { return "Randomizer"; }
  const char* getHelpLine() const override { return "GO/ENT:ROLL  <-/->:MODE  UP/DN:RANGE"; }
private:
  uint8_t mode_ = 0;
  int max_ = 100;
  String result_ = "press GO";
  String listItems_[32];
  uint8_t listCount_ = 0;
  void loadList();
};

class BrowserApp : public App {
public:
  void begin(AppContext& context) override;
  void draw() override;
  void onInput(const InputEvent& event) override;
  InputContext inputContext() const override { return entering_ ? InputContext::TextEntry : InputContext::Navigation; }
  const char* getTitle() const override { return "Text Browser"; }
  const char* getHelpLine() const override { return entering_ ? "TYPE URL  ENT:LOAD  HOLD GO:BACK" : "ENT:URL  UP/DN:SCROLL  GO:SAVE"; }
private:
  TextEditor url_;
  bool entering_ = true;
  bool selectingLink_ = false;
  String page_ = "Enter URL or search query.";
  uint16_t scroll_ = 0;
  String links_[16];
  String linkText_[16];
  uint8_t linkCount_ = 0;
  uint8_t selectedLink_ = 0;
  String history_[8];
  uint8_t historyCount_ = 0;
  void load();
  void loadUrl(const String& target);
  void parseHtml(const String& html, const String& base);
  String absoluteUrl(const String& href, const String& base) const;
};

class AIApp : public App {
public:
  void begin(AppContext& context) override;
  void draw() override;
  void onInput(const InputEvent& event) override;
  InputContext inputContext() const override { return InputContext::TextEntry; }
  const char* getTitle() const override { return "AI Text"; }
  const char* getHelpLine() const override { return "TYPE PROMPT  ENT:SEND  UP/DN:SCROLL"; }
private:
  TextEditor prompt_;
  String response_ = "Configure /config/ai.json with endpoint and apiKey.";
  uint16_t scroll_ = 0;
  void send();
  String extractResponse(const String& body);
};

class PaymentsApp : public App {
public:
  void draw() override;
  void onInput(const InputEvent&) override {}
  const char* getTitle() const override { return "Payments Info"; }
  const char* getHelpLine() const override { return "HOLD GO:MENU"; }
};

class InputDiagnosticsApp : public App {
public:
  void draw() override;
  void onInput(const InputEvent& event) override;
  const char* getTitle() const override { return "Input Test"; }
  const char* getHelpLine() const override { return "ENT:SCREEN-OFF  HOLD GO:MENU"; }

private:
  InputEvent last_;
  bool hasLast_ = false;
  const char* actionName(InputAction action) const;
  const char* typeName(InputEventType type) const;
};

class SystemInfoApp : public App {
public:
  void draw() override;
  void onInput(const InputEvent& event) override;
  const char* getTitle() const override { return "System Info"; }
  const char* getHelpLine() const override { return "GO:SD-RETRY  HOLD GO:BACK"; }
};
