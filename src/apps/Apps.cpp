#include "Apps.h"

#include "NetworkManager.h"
#include "PowerManager.h"
#include "SettingsManager.h"
#include "StorageManager.h"
#include "TerminalUI.h"
#include <AudioFileSourceID3.h>
#include <AudioFileSourceSD.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutput.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <M5Cardputer.h>
#include <WiFiClientSecure.h>
#include <time.h>

class AudioOutputM5Speaker : public AudioOutput {
public:
  AudioOutputM5Speaker(m5::Speaker_Class* speaker, uint8_t channel = 0) : speaker_(speaker), channel_(channel) {}
  bool begin() override { speaker_->begin(); return true; }
  bool ConsumeSample(int16_t sample[2]) override {
    if (index_ + 2 >= BufferSamples) flush();
    buffer_[buf_][index_++] = sample[0];
    buffer_[buf_][index_++] = sample[1];
    return true;
  }
  void flush() override {
    if (!index_) return;
    speaker_->playRaw(buffer_[buf_], index_, hertz, true, 1, channel_);
    buf_ = (buf_ + 1) % 3;
    index_ = 0;
  }
  bool stop() override {
    flush();
    speaker_->stop(channel_);
    return true;
  }

private:
  static constexpr size_t BufferSamples = 1536;
  m5::Speaker_Class* speaker_;
  uint8_t channel_;
  int16_t buffer_[3][BufferSamples];
  size_t index_ = 0;
  uint8_t buf_ = 0;
};

static bool pressed(const InputEvent& e, InputAction a) {
  return e.action == a && (e.type == InputEventType::Press || e.type == InputEventType::Repeat);
}

static String urlEncode(const String& in) {
  String out;
  const char* hex = "0123456789ABCDEF";
  for (size_t i = 0; i < in.length(); ++i) {
    char c = in[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += c;
    else if (c == ' ') out += '+';
    else { out += '%'; out += hex[(c >> 4) & 0xf]; out += hex[c & 0xf]; }
  }
  return out;
}

static String stripTags(String html) {
  html.replace("\r", "");
  String out;
  bool tag = false;
  bool skip = false;
  String tagName;
  for (size_t i = 0; i < html.length() && out.length() < 9000; ++i) {
    char c = html[i];
    if (c == '<') {
      tag = true;
      tagName = "";
      for (size_t j = i + 1; j < html.length() && j < i + 12; ++j) {
        char t = tolower(html[j]);
        if (!isalpha(t) && t != '/') break;
        tagName += t;
      }
      if (tagName.startsWith("script") || tagName.startsWith("style")) skip = true;
      if (tagName.startsWith("/script") || tagName.startsWith("/style")) skip = false;
      if (tagName.indexOf("br") >= 0 || tagName.indexOf("p") >= 0 || tagName.indexOf("li") >= 0) out += '\n';
      continue;
    }
    if (c == '>') { tag = false; continue; }
    if (!tag && !skip) out += c;
  }
  out.replace("&nbsp;", " ");
  out.replace("&amp;", "&");
  out.replace("&lt;", "<");
  out.replace("&gt;", ">");
  return out;
}

static bool storageReady(const AppContext* ctx) {
  return ctx && ctx->storage && ctx->storage->isMounted();
}

static void writeLe16(File& f, uint16_t v) { f.write(v & 0xff); f.write((v >> 8) & 0xff); }
static void writeLe32(File& f, uint32_t v) {
  f.write(v & 0xff); f.write((v >> 8) & 0xff); f.write((v >> 16) & 0xff); f.write((v >> 24) & 0xff);
}

void MusicApp::begin(AppContext& context) {
  App::begin(context);
  if (!ctx_->storage) {
    state_ = State::NoSD;
    return;
  }
  files_.begin(*ctx_->storage, "/music", ".mp3");
  volume_ = ctx_->settings ? ctx_->settings->get().volume : volume_;
  shuffle_ = ctx_->settings ? ctx_->settings->get().shuffle : shuffle_;
  state_ = State::NoSD;
  refreshLibrary();
}

void MusicApp::update() {
  if (!ctx_ || state_ != State::Ready || !storageReady(ctx_)) return;
  if (playing_ && mp3_) {
    if (mp3_->isRunning()) {
      if (!mp3_->loop()) nextTrack();
    } else {
      nextTrack();
    }
  }
  if (playing_ && millis() - vizTick_ > 120) {
    vizTick_ = millis();
    viz_ = (viz_ + 1) % 12;
  }
}

void MusicApp::draw() {
  if (!ctx_ || !ctx_->ui) return;
  ctx_->ui->header("Music");
  if (state_ == State::NoSD) {
    ctx_->ui->line(2, "SD not mounted");
    ctx_->ui->line(3, "GO Retry");
    ctx_->ui->line(4, "HOLD Back");
    return;
  }
  if (state_ == State::DirMissing) {
    ctx_->ui->line(2, "Music directory missing");
    ctx_->ui->line(4, "Unable to create /music");
    return;
  }
  uint16_t count = files_.count();
  if (state_ == State::Empty || !count) {
    ctx_->ui->line(2, "No MP3 files");
    ctx_->ui->line(4, "Put files in /music");
    return;
  }

  String filename = files_.selectedName();
  if (filename.length() > 26) filename = filename.substring(filename.length() - 26);

  ctx_->ui->line(2, String("Track ") + (files_.selected() + 1) + "/" + count);
  ctx_->ui->line(3, filename, TerminalUI::White);
  ctx_->ui->line(5, String("Status: ") + (playing_ ? "PLAY" : "PAUSE") +
                       String("  Vol:") + volume_ +
                       String("  Shuffle:") + (shuffle_ ? "ON" : "OFF"));

  ctx_->ui->drawValueBar(8, 86, 220, volume_, 15);
  String bars;
  for (uint8_t i = 0; i < 12; ++i) bars += (i <= viz_ && playing_) ? "|" : ".";
  ctx_->ui->line(12, bars, TerminalUI::Green);
  ctx_->ui->line(10, status_, status_.startsWith("error") ? TerminalUI::Red : TerminalUI::Dim);
}

void MusicApp::onInput(const InputEvent& e) {
  if (!ctx_ || !ctx_->storage) return;
  if (state_ == State::NoSD || state_ == State::DirMissing) {
    if (pressed(e, InputAction::Select) || pressed(e, InputAction::Enter)) {
      refreshLibrary();
    }
    return;
  }
  if (!files_.count()) {
    if (pressed(e, InputAction::Select) || pressed(e, InputAction::Enter)) refreshLibrary();
    return;
  }
  if (pressed(e, InputAction::Select) || pressed(e, InputAction::Enter)) {
    if (playing_) playing_ = false;
    else if (mp3_) playing_ = true;
    else playing_ = startTrack();
  }
  else if (pressed(e, InputAction::Right)) nextTrack();
  else if (pressed(e, InputAction::Left)) shuffle_ = !shuffle_;
  else if (pressed(e, InputAction::Up) && volume_ < 15) { ++volume_; applyVolume(); }
  else if (pressed(e, InputAction::Down) && volume_ > 0) { --volume_; applyVolume(); }
  if (ctx_->settings) {
    ctx_->settings->edit().volume = volume_;
    ctx_->settings->edit().shuffle = shuffle_;
    ctx_->settings->save();
  }
}

bool MusicApp::startTrack() {
  if (!ctx_ || !storageReady(ctx_)) { status_ = "error: no storage"; return false; }
  if (state_ != State::Ready || !files_.count()) { status_ = "error: no mp3 files"; return false; }
  stopTrack();
  const String path = files_.selectedPath();
  if (!ctx_->storage->isMounted() || !ctx_->storage->exists(path.c_str())) {
    status_ = "error: file missing";
    return false;
  }
  if (path.isEmpty()) { status_ = "error: invalid track"; return false; }
  file_ = new AudioFileSourceSD(path.c_str());
  if (!file_) { status_ = "error: file alloc failed"; return false; }
  id3_ = new AudioFileSourceID3(file_);
  if (!id3_) { stopTrack(); status_ = "error: id3 alloc failed"; return false; }
  mp3_ = new AudioGeneratorMP3();
  if (!mp3_) { stopTrack(); status_ = "error: decoder alloc failed"; return false; }
  output_ = new AudioOutputM5Speaker(&M5Cardputer.Speaker, 0);
  if (!output_) {
    stopTrack();
    status_ = "error: speaker alloc failed";
    return false;
  }
  applyVolume();
  if (!mp3_->begin(id3_, output_)) {
    status_ = "error: MP3 open/output failed";
    if (ctx_->storage) ctx_->storage->log(status_ + " " + path);
    stopTrack();
    return false;
  }
  status_ = String("playing ") + path;
  playing_ = true;
  return true;
}

void MusicApp::stopTrack() {
  if (mp3_) { mp3_->stop(); delete mp3_; mp3_ = nullptr; }
  if (id3_) { delete id3_; id3_ = nullptr; }
  if (file_) { file_->close(); delete file_; file_ = nullptr; }
  if (output_) { delete output_; output_ = nullptr; }
  playing_ = false;
}

void MusicApp::nextTrack() {
  if (!ctx_ || state_ != State::Ready || !files_.count()) return;
  if (shuffle_ && files_.count() > 1) files_.move(random(1, files_.count()));
  else files_.move(1);
  startTrack();
}

void MusicApp::applyVolume() {
  M5Cardputer.Speaker.setVolume(map(volume_, 0, 15, 0, 255));
}

void MusicApp::refreshLibrary() {
  status_ = "idle";
  if (!ctx_ || !ctx_->storage || !ctx_->storage->isMounted()) {
    state_ = State::NoSD;
    stopTrack();
    return;
  }
  bool hasDir = ctx_->storage->exists("/music");
  if (!hasDir) hasDir = ctx_->storage->makeDir("/music");
  if (!hasDir) {
    state_ = State::DirMissing;
    return;
  }
  files_.refresh();
  state_ = files_.count() ? State::Ready : State::Empty;
}

void RecorderApp::begin(AppContext& context) {
  App::begin(context);
  M5Cardputer.Mic.begin();
  if (!storageReady(ctx_)) {
    recordingCount_ = 0;
    return;
  }
  path_ = makeRecordingPath();
  String files[128];
  recordingCount_ = ctx_->storage->listFiles("/recordings", ".wav", files, 128);
}

void RecorderApp::update() {
  if (!ctx_ || !storageReady(ctx_) || !recording_ || paused_) return;
  if (M5Cardputer.Mic.isRecording()) return;
  if (M5Cardputer.Mic.record(chunk_, ChunkSamples, SampleRate, false)) {
    if (!file_) return;
    file_.write(reinterpret_cast<const uint8_t*>(chunk_), ChunkSamples * sizeof(int16_t));
    dataBytes_ += ChunkSamples * sizeof(int16_t);
    elapsedMs_ = millis() - startedAt_;
  } else {
    if (ctx_->storage) ctx_->storage->log("recording error: Mic.record failed");
  }
}

void RecorderApp::draw() {
  if (!ctx_ || !ctx_->ui) return;
  ctx_->ui->header("Recorder");
  if (!storageReady(ctx_)) {
    ctx_->ui->line(2, "SD REQUIRED");
    ctx_->ui->line(3, "Insert card or retry");
    return;
  }
  ctx_->ui->line(2, String("State: ") + (recording_ ? (paused_ ? "paused" : "recording") : "stopped"));
  ctx_->ui->line(4, String("Elapsed: ") + ((recording_ ? elapsedMs_ : 0) / 1000) + "s");
  ctx_->ui->line(6, String("Target: ") + path_);
  ctx_->ui->line(8, String("Recordings: ") + recordingCount_ + "  Bytes:" + dataBytes_);
  ctx_->ui->line(10, "PCM WAV mono 16-bit 16 kHz", TerminalUI::Dim);
}

void RecorderApp::onInput(const InputEvent& e) {
  if (!ctx_) return;
  if (pressed(e, InputAction::Select)) {
    if (!storageReady(ctx_)) return;
    if (!recording_) {
      startRecording();
    } else {
      paused_ = !paused_;
    }
  } else if (pressed(e, InputAction::Enter) && recording_) {
    stopRecording();
  }
}

void RecorderApp::startRecording() {
  if (!ctx_ || !storageReady(ctx_)) {
    if (ctx_ && ctx_->storage) ctx_->storage->log("recording error: SD missing");
    return;
  }
  path_ = makeRecordingPath();
  file_ = ctx_->storage->open(path_.c_str(), FILE_WRITE);
  if (!file_) { ctx_->storage->log("recording error: open failed " + path_); return; }
  dataBytes_ = 0;
  elapsedMs_ = 0;
  writeWavHeader(0);
  recording_ = true;
  paused_ = false;
  startedAt_ = millis();
}

void RecorderApp::stopRecording() {
  if (!ctx_ || !storageReady(ctx_) || !recording_) return;
  if (M5Cardputer.Mic.isRecording()) delay(2);
  elapsedMs_ = millis() - startedAt_;
  if (!file_) return;
  file_.seek(0);
  writeWavHeader(dataBytes_);
  file_.close();
  recording_ = false;
  paused_ = false;
  ++recordingCount_;
  path_ = makeRecordingPath();
}

void RecorderApp::writeWavHeader(uint32_t dataBytes) {
  file_.write(reinterpret_cast<const uint8_t*>("RIFF"), 4);
  writeLe32(file_, 36 + dataBytes);
  file_.write(reinterpret_cast<const uint8_t*>("WAVEfmt "), 8);
  writeLe32(file_, 16);
  writeLe16(file_, 1);
  writeLe16(file_, 1);
  writeLe32(file_, SampleRate);
  writeLe32(file_, SampleRate * sizeof(int16_t));
  writeLe16(file_, sizeof(int16_t));
  writeLe16(file_, 16);
  file_.write(reinterpret_cast<const uint8_t*>("data"), 4);
  writeLe32(file_, dataBytes);
}

String RecorderApp::makeRecordingPath() {
  struct tm tmInfo;
  if (getLocalTime(&tmInfo, 10) && tmInfo.tm_year > 120) {
    char name[40];
    strftime(name, sizeof(name), "/recordings/REC_%Y%m%d_%H%M%S.wav", &tmInfo);
    return String(name);
  }
  return ctx_->storage->nextNumberedPath("/recordings", "REC_", ".wav");
}

void NotesApp::begin(AppContext& context) {
  App::begin(context);
  if (!storageReady(ctx_)) return;
  files_.begin(*ctx_->storage, "/notes", ".txt");
}

void NotesApp::update() {
  if (!ctx_ || !storageReady(ctx_)) return;
  if (editing_ && editor_.dirty() && millis() - lastAutosave_ > 15000) {
    ctx_->storage->writeText("/notes/.draft.txt", editor_.text());
    lastAutosave_ = millis();
  }
}

void NotesApp::draw() {
  if (!ctx_ || !ctx_->ui) return;
  ctx_->ui->header("Notes");
  if (editing_) {
    ctx_->ui->line(2, path_, TerminalUI::Green);
    uint8_t row = 4;
    String text = editor_.text();
    uint16_t start = text.length() > 240 ? text.length() - 240 : 0;
    for (uint8_t i = 0; i < 8; ++i) {
      String part = text.substring(start + i * 32, start + (i + 1) * 32);
      part.replace('\n', ' ');
      ctx_->ui->line(row + i, part);
    }
    return;
  }
  if (!storageReady(ctx_)) {
    ctx_->ui->line(2, "SD REQUIRED");
    ctx_->ui->line(3, "Insert card or retry");
    return;
  }
  if (!files_.count()) ctx_->ui->line(2, "No notes. Press N to create.", TerminalUI::Yellow);
  for (uint8_t i = 0; i < 10 && files_.top() + i < files_.count(); ++i) {
    uint16_t idx = files_.top() + i;
    ctx_->ui->listItem(i + 2, files_.item(idx), idx == files_.selected());
  }
}

void NotesApp::onInput(const InputEvent& e) {
  if (!ctx_) return;
  if (!storageReady(ctx_) && (pressed(e, InputAction::Select) || pressed(e, InputAction::Enter) ||
                             e.action == InputAction::TextChar)) {
    return;
  }
  if (editing_) {
    if (pressed(e, InputAction::Select)) save();
    else editor_.onInput(e, true);
    return;
  }
  if (pressed(e, InputAction::Up)) files_.move(-1);
  else if (pressed(e, InputAction::Down)) files_.move(1);
  else if (pressed(e, InputAction::Select) || pressed(e, InputAction::Enter)) openSelected();
  else if (e.action == InputAction::TextChar && (e.text == 'n' || e.text == 'N')) newNote();
}

void NotesApp::openSelected() {
  if (!ctx_ || !storageReady(ctx_)) return;
  if (!files_.count()) { newNote(); return; }
  path_ = files_.selectedPath();
  editor_.setText(ctx_->storage->readText(path_.c_str(), 16000));
  editing_ = true;
}

void NotesApp::save() {
  if (!ctx_ || !storageReady(ctx_)) return;
  if (path_.isEmpty()) path_ = ctx_->storage->nextNumberedPath("/notes", "NOTE_", ".txt");
  ctx_->storage->writeText(path_.c_str(), editor_.text());
  editor_.markSaved();
  files_.refresh();
}

void NotesApp::newNote() {
  if (!ctx_ || !storageReady(ctx_)) return;
  path_ = ctx_->storage->nextNumberedPath("/notes", "NOTE_", ".txt");
  editor_.clear();
  editing_ = true;
}

void ReaderApp::begin(AppContext& context) {
  App::begin(context);
  if (!storageReady(ctx_)) return;
  files_.begin(*ctx_->storage, "/books", ".txt");
}

void ReaderApp::draw() {
  if (!ctx_ || !ctx_->ui) return;
  ctx_->ui->header("Reader");
  if (!storageReady(ctx_)) {
    ctx_->ui->line(2, "SD REQUIRED");
    ctx_->ui->line(3, "Insert card or retry");
    return;
  }
  if (!opened_) {
    if (!files_.count()) ctx_->ui->line(2, "No .txt books in /books", TerminalUI::Yellow);
    for (uint8_t i = 0; i < 10 && files_.top() + i < files_.count(); ++i) {
      uint16_t idx = files_.top() + i;
      ctx_->ui->listItem(i + 2, files_.item(idx), idx == files_.selected());
    }
    return;
  }
  ctx_->ui->line(2, book_, TerminalUI::Green);
  if (speedMode_) {
    ctx_->ui->line(6, text_.substring(pos_, min<uint16_t>(text_.length(), pos_ + 24)), TerminalUI::Yellow);
    ctx_->ui->line(9, String("WPM: ") + wpm_ + (paused_ ? " paused" : " running"));
  } else {
    for (uint8_t i = 0; i < 9; ++i) ctx_->ui->line(i + 4, text_.substring(pos_ + i * 32, pos_ + (i + 1) * 32));
  }
}

void ReaderApp::onInput(const InputEvent& e) {
  if (!ctx_) return;
  if (!storageReady(ctx_)) return;
  if (!opened_) {
    if (pressed(e, InputAction::Up)) files_.move(-1);
    else if (pressed(e, InputAction::Down)) files_.move(1);
    else if ((pressed(e, InputAction::Select) || pressed(e, InputAction::Enter)) && files_.count()) {
      book_ = files_.selectedPath();
      text_ = ctx_->storage->readText(book_.c_str(), 48000);
      opened_ = true; pos_ = 0;
    }
    return;
  }
  if (pressed(e, InputAction::Enter)) speedMode_ = !speedMode_;
  else if (pressed(e, InputAction::Select)) paused_ = !paused_;
  else if (pressed(e, InputAction::Down)) pos_ = min<uint16_t>(text_.length(), pos_ + 32);
  else if (pressed(e, InputAction::Up)) pos_ = pos_ > 32 ? pos_ - 32 : 0;
  else if (pressed(e, InputAction::Right)) speedMode_ ? wpm_ += 25 : pos_ = min<uint16_t>(text_.length(), pos_ + 320);
  else if (pressed(e, InputAction::Left)) speedMode_ ? wpm_ = max<uint16_t>(100, wpm_ - 25) : pos_ = pos_ > 320 ? pos_ - 320 : 0;
}

void ClockApp::begin(AppContext& context) {
  App::begin(context);
  if (!ctx_ || !ctx_->settings) return;
  manualOffsetMin_ = ctx_->settings->get().timezoneOffsetMin;
}

void ClockApp::update() {
  if (!ctx_ || !ctx_->network || !ctx_->storage) return;
  if (ctx_->network->connected() && !ntpStarted_) syncNtp();
  if (running_) elapsedMs_ = millis() - baseMs_;
  if (mode_ == 2 && running_ && elapsedMs_ >= timerMs_ && !timerDone_) {
    timerDone_ = true;
    running_ = false;
    M5Cardputer.Speaker.begin();
    M5Cardputer.Speaker.tone(880, 250);
    ctx_->storage->log("timer finished");
  }
}

void ClockApp::draw() {
  if (!ctx_ || !ctx_->ui || !ctx_->network) return;
  ctx_->ui->header("Clock");
  const char* names[] = {"Clock", "Stopwatch", "Timer"};
  ctx_->ui->line(2, String("Mode: ") + names[mode_], TerminalUI::Green);
  if (mode_ == 0) {
    struct tm now;
    if (getLocalTime(&now, 5)) {
      char buf[32];
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &now);
      ctx_->ui->line(5, String(buf) + " Paris", TerminalUI::Yellow);
    } else {
      ctx_->ui->line(5, String("Manual/uptime: ") + (millis() / 1000 + manualOffsetMin_ * 60) + "s");
    }
    ctx_->ui->line(7, String("NTP: ") + (timeKnown_ ? "synced" : (ntpStarted_ ? "pending" : "not synced")));
  }
  else if (mode_ == 1) ctx_->ui->line(5, String("Stopwatch: ") + elapsedMs_ / 1000 + "s");
  else ctx_->ui->line(5, String("Timer: ") + max<int32_t>(0, (int32_t)(timerMs_ - elapsedMs_) / 1000) + "s");
}

void ClockApp::onInput(const InputEvent& e) {
  if (!ctx_) return;
  if (pressed(e, InputAction::Left) && mode_ > 0) --mode_;
  else if (pressed(e, InputAction::Right) && mode_ < 2) ++mode_;
  else if (pressed(e, InputAction::Select)) {
    running_ = !running_;
    baseMs_ = millis() - elapsedMs_;
  } else if (pressed(e, InputAction::Enter)) {
    running_ = false; elapsedMs_ = 0; baseMs_ = millis(); timerDone_ = false;
    if (mode_ == 0) syncNtp();
  } else if (mode_ == 2 && pressed(e, InputAction::Up)) timerMs_ += 60000;
  else if (mode_ == 2 && pressed(e, InputAction::Down)) timerMs_ = timerMs_ > 60000 ? timerMs_ - 60000 : 60000;
}

void ClockApp::syncNtp() {
  if (!ctx_ || !ctx_->network) return;
  if (!ctx_->network->connected()) return;
  setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
  tzset();
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  ntpStarted_ = true;
  struct tm now;
  timeKnown_ = getLocalTime(&now, 100);
}

void NetworkApp::begin(AppContext& context) {
  App::begin(context);
}

void NetworkApp::draw() {
  if (!ctx_ || !ctx_->ui || !ctx_->network) return;
  ctx_->ui->header("Network");
  String live = ctx_->network->connected() ? "connected" : (ctx_->network->connecting() ? "connecting" : (ctx_->network->failed() ? "failed" : "disconnected"));
  ctx_->ui->line(2, String("IP: ") + ctx_->network->ip() + " RSSI:" + ctx_->network->rssi(), TerminalUI::Green);
  ctx_->ui->line(3, String("Status: ") + live + " " + status_, TerminalUI::Yellow);
  if (password_) {
    ctx_->ui->line(5, String("SSID: ") + ssid_);
    ctx_->ui->line(7, String("Password: ") + String(pass_.text().length(), '*'));
    return;
  }
  if (!scanCount_) ctx_->ui->line(5, "GO scan. LEFT reconnect saved.");
  for (uint8_t i = 0; i < 8 && i < scanCount_; ++i) {
    String item = ctx_->network->scannedSsid(i) + (ctx_->network->scannedOpen(i) ? " open " : " lock ");
    item += ctx_->network->scannedRssi(i);
    ctx_->ui->listItem(i + 5, item, i == selected_);
  }
}

void NetworkApp::onInput(const InputEvent& e) {
  if (!ctx_ || !ctx_->network) return;
  if (password_) {
    if (pressed(e, InputAction::Enter)) {
      bool connected = ctx_->network->connect(ssid_, pass_.text());
      status_ = connected ? "connected" : "connecting";
      password_ = false; pass_.clear();
    } else pass_.onInput(e, false);
    return;
  }
  if (pressed(e, InputAction::Select)) { scanCount_ = ctx_->network->scan(); selected_ = 0; status_ = "scan complete"; }
  else if (pressed(e, InputAction::Left)) { ctx_->network->reconnectKnown(); status_ = "reconnect saved"; }
  else if (pressed(e, InputAction::Up) && selected_ > 0) --selected_;
  else if (pressed(e, InputAction::Down) && selected_ < scanCount_ - 1) ++selected_;
  else if (pressed(e, InputAction::Enter) && scanCount_ > 0) { ssid_ = ctx_->network->scannedSsid(selected_); password_ = true; pass_.clear(); }
}

void WebFileManagerApp::begin(AppContext& context) {
  App::begin(context);
  if (!ctx_->storage || !ctx_->network) return;
  server_.on("/", HTTP_GET, [this]() { handleRoot(); });
  server_.on("/api/list", HTTP_GET, [this]() { handleList(); });
  server_.on("/download", HTTP_GET, [this]() { handleDownload(); });
  server_.on("/upload", HTTP_POST, [this]() { server_.send(200, "application/json", "{\"ok\":true}"); }, [this]() { handleUpload(); });
  server_.on("/mkdir", HTTP_POST, [this]() { handleMkdir(); });
  server_.on("/delete", HTTP_POST, [this]() { handleDelete(); });
}

void WebFileManagerApp::update() {
  if (running_) server_.handleClient();
}

void WebFileManagerApp::draw() {
  if (!ctx_ || !ctx_->ui) return;
  ctx_->ui->header("Web File Manager");
  if (!ctx_->storage || !ctx_->storage->isMounted()) {
    ctx_->ui->line(2, "SD REQUIRED");
    ctx_->ui->line(3, "Insert card or retry");
    return;
  }
  ctx_->ui->line(2, String("State: ") + (running_ ? "running" : "stopped"));
  ctx_->ui->line(4, String("URL: http://") + ctx_->network->ip() + "/");
  ctx_->ui->line(6, "Local network only. Auth optional via settings.", TerminalUI::Yellow);
}

void WebFileManagerApp::onInput(const InputEvent& e) {
  if (!ctx_ || !ctx_->storage || !ctx_->storage->isMounted()) return;
  if (pressed(e, InputAction::Select)) running_ ? stop() : start();
}

void WebFileManagerApp::start() {
  if (!ctx_ || !ctx_->network || !ctx_->network->connected() || !ctx_->storage || !ctx_->storage->isMounted()) return;
  server_.begin();
  running_ = true;
}

void WebFileManagerApp::stop() {
  server_.stop();
  running_ = false;
}

String WebFileManagerApp::cleanPath(const String& raw, bool* ok) const {
  String p = raw.length() ? raw : "/";
  p.replace("\\", "/");
  while (p.indexOf("//") >= 0) p.replace("//", "/");
  if (!p.startsWith("/")) p = "/" + p;
  bool good = p.indexOf("..") < 0 && p.indexOf('\0') < 0;
  if (ok) *ok = good;
  return good ? p : "/";
}

void WebFileManagerApp::handleRoot() {
  String body = "<html><body><h1>Cardputer SD</h1>"
                "<form method='POST' action='/upload?path=/notes' enctype='multipart/form-data'><input type='file' name='f'><button>upload notes</button></form>"
                "<ul><li><a href='/api/list?path=/'>JSON root</a></li>"
                "<li>/music</li><li>/recordings</li><li>/notes</li><li>/books</li></ul>"
                "<p>Unauthenticated local network server.</p></body></html>";
  server_.send(200, "text/html", body);
}

void WebFileManagerApp::handleList() {
  bool ok = false;
  String path = cleanPath(server_.arg("path"), &ok);
  if (!ok) { server_.send(400, "application/json", "{\"error\":\"bad path\"}"); return; }
  if (!ctx_ || !ctx_->storage || !ctx_->storage->isMounted() || !ctx_->storage->isDir(path.c_str())) {
    server_.send(404, "application/json", "{\"error\":\"not directory\"}");
    return;
  }
  File dir = ctx_->storage->open(path.c_str(), FILE_READ);
  if (!dir || !dir.isDirectory()) { server_.send(404, "application/json", "{\"error\":\"not directory\"}"); return; }
  JsonDocument doc;
  JsonArray arr = doc["items"].to<JsonArray>();
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    JsonObject item = arr.add<JsonObject>();
    item["name"] = String(f.name());
    item["dir"] = f.isDirectory();
    item["size"] = f.size();
  }
  String out;
  serializeJson(doc, out);
  server_.send(200, "application/json", out);
}

void WebFileManagerApp::handleDownload() {
  bool ok = false;
  String path = cleanPath(server_.arg("path"), &ok);
  if (!ok) { server_.send(400, "text/plain", "bad path"); return; }
  if (!ctx_ || !ctx_->storage || !ctx_->storage->isMounted()) {
    server_.send(500, "text/plain", "storage unavailable");
    return;
  }
  File f = ctx_->storage->open(path.c_str(), FILE_READ);
  if (!f || f.isDirectory()) { server_.send(404, "text/plain", "not found"); return; }
  server_.streamFile(f, "application/octet-stream");
  f.close();
}

void WebFileManagerApp::handleUpload() {
  HTTPUpload& upload = server_.upload();
  bool ok = false;
  String dir = cleanPath(server_.arg("path"), &ok);
  if (!ok || !ctx_ || !ctx_->storage || !ctx_->storage->isMounted()) return;
  if (upload.status == UPLOAD_FILE_START) {
    String name = upload.filename;
    name.replace("/", "_");
    uploadPath_ = dir + "/" + name;
    uploadFile_ = ctx_->storage->open(uploadPath_.c_str(), FILE_WRITE);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile_) uploadFile_.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile_) uploadFile_.close();
  }
}

void WebFileManagerApp::handleMkdir() {
  bool ok = false;
  String path = cleanPath(server_.arg("path"), &ok);
  if (!ok) { server_.send(400, "application/json", "{\"error\":\"bad path\"}"); return; }
  if (!ctx_ || !ctx_->storage || !ctx_->storage->isMounted()) {
    server_.send(500, "application/json", "{\"error\":\"storage unavailable\"}");
    return;
  }
  bool done = ctx_->storage->makeDir(path.c_str());
  server_.send(done ? 200 : 500, "application/json", done ? "{\"ok\":true}" : "{\"error\":\"mkdir failed\"}");
}

void WebFileManagerApp::handleDelete() {
  bool ok = false;
  String path = cleanPath(server_.arg("path"), &ok);
  if (!ok || path == "/") { server_.send(400, "application/json", "{\"error\":\"bad path\"}"); return; }
  if (!ctx_ || !ctx_->storage || !ctx_->storage->isMounted()) {
    server_.send(500, "application/json", "{\"error\":\"storage unavailable\"}");
    return;
  }
  File f = ctx_->storage->open(path.c_str(), FILE_READ);
  if (!f) { server_.send(404, "application/json", "{\"error\":\"not found\"}"); return; }
  bool dir = f.isDirectory();
  f.close();
  bool done = dir ? ctx_->storage->rmdir(path.c_str()) : ctx_->storage->erase(path.c_str());
  server_.send(done ? 200 : 500, "application/json", done ? "{\"ok\":true}" : "{\"error\":\"delete failed\"}");
}

void RandomizerApp::draw() {
  if (!ctx_ || !ctx_->ui) return;
  ctx_->ui->header("Randomizer");
  const char* modes[] = {"Yes/No/Maybe", "Number Range", "List Picker"};
  ctx_->ui->line(2, String("Mode: ") + modes[mode_], TerminalUI::Green);
  ctx_->ui->line(4, String("Range max: ") + max_);
  if (mode_ == 2) ctx_->ui->line(5, String("List items: ") + listCount_);
  ctx_->ui->line(6, String("Result: ") + result_, TerminalUI::Yellow);
}

void RandomizerApp::onInput(const InputEvent& e) {
  if (pressed(e, InputAction::Left) && mode_ > 0) --mode_;
  else if (pressed(e, InputAction::Right) && mode_ < 2) ++mode_;
  else if (pressed(e, InputAction::Up)) ++max_;
  else if (pressed(e, InputAction::Down) && max_ > 2) --max_;
  else if (pressed(e, InputAction::Select) || pressed(e, InputAction::Enter)) {
    if (mode_ == 0) { const char* r[] = {"Yes", "No", "Maybe"}; result_ = r[random(0, 3)]; }
    else if (mode_ == 1) result_ = String(random(1, max_ + 1));
    else {
      if (!listCount_) loadList();
      result_ = listCount_ ? listItems_[random(0, listCount_)] : "No /config/randomizer_lists.txt";
    }
  }
}

void RandomizerApp::loadList() {
  if (!ctx_ || !storageReady(ctx_)) {
    result_ = "SD REQUIRED";
    return;
  }
  listCount_ = 0;
  String text = ctx_->storage->readText("/config/randomizer_lists.txt", 12000);
  if (!text.length()) text = ctx_->storage->readText("/notes/randomizer.txt", 12000);
  if (!text.length()) {
    result_ = "No /config/randomizer_lists.txt";
    return;
  }
  int start = 0;
  while (start < static_cast<int>(text.length()) && listCount_ < 32) {
    int end = text.indexOf('\n', start);
    if (end < 0) end = text.length();
    String line = text.substring(start, end);
    line.trim();
    if (line.length() && !line.startsWith("#")) listItems_[listCount_++] = line;
    start = end + 1;
  }
}

void BrowserApp::begin(AppContext& context) {
  App::begin(context);
  url_.setText("https://example.com");
}

void BrowserApp::draw() {
  if (!ctx_ || !ctx_->ui) return;
  ctx_->ui->header("Text Browser");
  if (entering_) ctx_->ui->line(3, String("URL/search: ") + url_.visibleLine(34), TerminalUI::Yellow);
  else if (selectingLink_) {
    ctx_->ui->line(2, String("Links: ") + linkCount_, TerminalUI::Green);
    if (!linkCount_) ctx_->ui->line(4, "No links found.", TerminalUI::Yellow);
    for (uint8_t i = 0; i < 9 && i < linkCount_; ++i) ctx_->ui->listItem(i + 4, linkText_[i], i == selectedLink_);
  }
  else {
    for (uint8_t i = 0; i < 10; ++i) ctx_->ui->line(i + 2, page_.substring(scroll_ + i * 34, scroll_ + (i + 1) * 34));
  }
}

void BrowserApp::onInput(const InputEvent& e) {
  if (!ctx_ || !ctx_->storage || !ctx_->storage->isMounted() || !ctx_->network) return;
  if (entering_) {
    if (pressed(e, InputAction::Enter)) load();
    else url_.onInput(e, false);
    return;
  }
  if (selectingLink_) {
    if (pressed(e, InputAction::Up) && selectedLink_ > 0) --selectedLink_;
    else if (pressed(e, InputAction::Down) && selectedLink_ + 1 < linkCount_) ++selectedLink_;
    else if ((pressed(e, InputAction::Enter) || pressed(e, InputAction::Right)) && linkCount_) loadUrl(links_[selectedLink_]);
    else if (pressed(e, InputAction::Left)) selectingLink_ = false;
    return;
  }
  if (pressed(e, InputAction::Enter)) entering_ = true;
  else if (pressed(e, InputAction::Right)) { selectingLink_ = true; selectedLink_ = 0; }
  else if (pressed(e, InputAction::Left) && historyCount_ > 1) { --historyCount_; loadUrl(history_[historyCount_ - 1]); }
  else if (pressed(e, InputAction::Down)) scroll_ = min<uint16_t>(page_.length(), scroll_ + 34);
  else if (pressed(e, InputAction::Up)) scroll_ = scroll_ > 34 ? scroll_ - 34 : 0;
  else if (pressed(e, InputAction::Select)) {
    ctx_->storage->writeText("/browser/saved_pages/page.txt", page_);
  }
}

void BrowserApp::load() {
  if (!ctx_ || !ctx_->network || !ctx_->network->connected()) {
    page_ = "Wi-Fi not connected.";
    entering_ = false;
    return;
  }
  String target = url_.text();
  if (!target.startsWith("http")) target = "https://duckduckgo.com/html/?q=" + urlEncode(target);
  loadUrl(target);
}

void BrowserApp::loadUrl(const String& target) {
  if (!ctx_ || !ctx_->network) return;
  if (!ctx_->network->connected()) {
    page_ = "Wi-Fi not connected.";
    entering_ = false;
    return;
  }
  HTTPClient http;
  WiFiClientSecure secure;
  secure.setInsecure();
  bool ok = target.startsWith("https://") ? http.begin(secure, target) : http.begin(target);
  if (!ok) { page_ = "Unsupported URL or TLS setup failed."; entering_ = false; return; }
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(8000);
  int code = http.GET();
  if (code <= 0) page_ = "Browser request failed.";
  else if (code >= 400) page_ = String("HTTP error ") + code;
  else {
    String body = http.getString();
    if (body.length() > 16000) body = body.substring(0, 16000);
    parseHtml(body, target);
    if (historyCount_ < 8) history_[historyCount_++] = target;
  }
  http.end();
  scroll_ = 0; entering_ = false; selectingLink_ = false;
}

void BrowserApp::parseHtml(const String& html, const String& base) {
  linkCount_ = 0;
  int pos = 0;
  while (linkCount_ < 16) {
    int href = html.indexOf("href=", pos);
    if (href < 0) href = html.indexOf("HREF=", pos);
    if (href < 0) break;
    int q = href + 5;
    while (q < static_cast<int>(html.length()) && html[q] == ' ') ++q;
    char quote = html[q];
    if (quote != '"' && quote != '\'') { pos = q + 1; continue; }
    int end = html.indexOf(quote, q + 1);
    if (end < 0) break;
    String hrefValue = html.substring(q + 1, end);
    if (hrefValue.startsWith("http") || hrefValue.startsWith("/")) {
      links_[linkCount_] = absoluteUrl(hrefValue, base);
      linkText_[linkCount_] = links_[linkCount_];
      if (linkText_[linkCount_].length() > 34) linkText_[linkCount_] = linkText_[linkCount_].substring(0, 34);
      ++linkCount_;
    }
    pos = end + 1;
  }
  page_ = stripTags(html);
  if (!page_.length()) page_ = "No readable text.";
}

String BrowserApp::absoluteUrl(const String& href, const String& base) const {
  if (href.startsWith("http")) return href;
  int scheme = base.indexOf("://");
  if (scheme < 0) return href;
  int slash = base.indexOf('/', scheme + 3);
  String origin = slash < 0 ? base : base.substring(0, slash);
  return origin + href;
}

void AIApp::begin(AppContext& context) {
  App::begin(context);
}

void AIApp::draw() {
  if (!ctx_ || !ctx_->ui) return;
  ctx_->ui->header("AI Text");
  ctx_->ui->line(2, String("Prompt: ") + prompt_.visibleLine(32), TerminalUI::Yellow);
  for (uint8_t i = 0; i < 8; ++i) ctx_->ui->line(i + 5, response_.substring(scroll_ + i * 34, scroll_ + (i + 1) * 34));
}

void AIApp::onInput(const InputEvent& e) {
  if (!ctx_) return;
  if (pressed(e, InputAction::Enter)) send();
  else if (pressed(e, InputAction::Down)) scroll_ = min<uint16_t>(response_.length(), scroll_ + 34);
  else if (pressed(e, InputAction::Up)) scroll_ = scroll_ > 34 ? scroll_ - 34 : 0;
  else prompt_.onInput(e, false);
}

void AIApp::send() {
  if (!ctx_ || !ctx_->network || !ctx_->storage) {
    response_ = "System unavailable.";
    return;
  }
  if (!ctx_->storage->isMounted()) {
    response_ = "SD not mounted.";
    return;
  }
  if (!ctx_->network->connected()) { response_ = "Wi-Fi not connected."; return; }
  String cfg = ctx_->storage->readText("/config/ai.json", 4096);
  if (!cfg.length()) { response_ = "ai.json missing."; return; }
  JsonDocument conf;
  if (deserializeJson(conf, cfg)) { response_ = "ai.json parse error."; return; }
  bool enabled = conf["enabled"] | true;
  String endpoint = conf["endpoint"] | "";
  String apiKey = conf["apiKey"] | "";
  String provider = conf["provider"] | "openai_compatible";
  String model = conf["model"] | "gpt-4o-mini";
  String systemPrompt = conf["systemPrompt"] | "You are a concise assistant.";
  bool insecure = conf["insecureTlsForTesting"] | true;
  if (!enabled) { response_ = "AI disabled in ai.json."; return; }
  if (!endpoint.length()) { response_ = "API endpoint missing."; return; }
  if (!apiKey.length()) { response_ = "API key missing."; return; }

  JsonDocument payload;
  if (provider == "openai_compatible") {
    payload["model"] = model;
    JsonArray messages = payload["messages"].to<JsonArray>();
    JsonObject sys = messages.add<JsonObject>();
    sys["role"] = "system"; sys["content"] = systemPrompt;
    JsonObject user = messages.add<JsonObject>();
    user["role"] = "user"; user["content"] = prompt_.text();
    payload["max_tokens"] = 512;
  } else {
    payload["prompt"] = prompt_.text();
    payload["model"] = model;
  }
  String body;
  serializeJson(payload, body);

  HTTPClient http;
  WiFiClientSecure secure;
  if (insecure) secure.setInsecure();
  bool ok = endpoint.startsWith("https://") ? http.begin(secure, endpoint) : http.begin(endpoint);
  if (!ok) { response_ = "HTTP setup failed."; return; }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + apiKey);
  http.setTimeout(15000);
  int code = http.POST(body);
  String resp = code > 0 ? http.getString() : "";
  http.end();
  if (code <= 0) { response_ = "HTTP request failed."; return; }
  if (code >= 400) { response_ = String("HTTP error ") + code + ": " + resp.substring(0, 500); return; }
  if (resp.length() > 12000) resp = resp.substring(0, 12000);
  response_ = extractResponse(resp);
  if (!response_.length()) response_ = "JSON parse error or empty response.";
  ctx_->storage->writeText("/ai/last_prompt.txt", prompt_.text());
  ctx_->storage->writeText("/ai/last_response.txt", response_);
}

String AIApp::extractResponse(const String& body) {
  JsonDocument doc;
  if (!deserializeJson(doc, body)) {
    const char* content = doc["choices"][0]["message"]["content"] | nullptr;
    if (content) return String(content);
    const char* text = doc["choices"][0]["text"] | nullptr;
    if (text) return String(text);
    const char* output = doc["output"] | nullptr;
    if (output) return String(output);
    const char* response = doc["response"] | nullptr;
    if (response) return String(response);
  }
  return body.substring(0, 4000);
}

void PaymentsApp::draw() {
  if (!ctx_ || !ctx_->ui) return;
  ctx_->ui->header("Payments Info");
  ctx_->ui->line(3, "Payments are not supported on this");
  ctx_->ui->line(4, "hardware/firmware. Google Pay and");
  ctx_->ui->line(5, "contactless payments require eligible");
  ctx_->ui->line(6, "Android NFC/HCE payment stack.");
  ctx_->ui->line(8, "No card data is stored or emulated.", TerminalUI::Yellow);
}

void InputDiagnosticsApp::draw() {
  if (!ctx_ || !ctx_->ui) return;
  ctx_->ui->header("Input Test");
  InputEvent e = ctx_->input ? ctx_->input->lastEvent() : InputEvent{};
  if (!hasLast_) {
    ctx_->ui->line(3, "Press any key or GO.");
    if (ctx_->input) {
      ctx_->ui->line(5, String("raw: ") + (ctx_->input->lastRawText().length() ? ctx_->input->lastRawText() : String("<none>")));
      ctx_->ui->line(6, String("fn: ") + String(ctx_->input->lastFnState() ? "on" : "off"));
    }
    return;
  }
  if (ctx_->input) {
    ctx_->ui->line(2, String("raw: ") + (ctx_->input->lastRawText().length() ? ctx_->input->lastRawText() : String("-")), TerminalUI::Dim);
    ctx_->ui->line(3, String("fn: ") + String(ctx_->input->lastFnState() ? "on" : "off"), TerminalUI::Dim);
  }
  ctx_->ui->line(4, String("Action: ") + actionName(e.action), TerminalUI::Green);
  ctx_->ui->line(5, String("Type: ") + typeName(e.type));
  ctx_->ui->line(6, String("Text: ") + (e.text ? String(e.text) : String("-")));
  ctx_->ui->line(7, String("Time: ") + e.timestamp);
  ctx_->ui->line(8, String("Wake suppressed: ") + (e.wakeSuppressed ? "yes" : "no"), TerminalUI::Yellow);
}

void InputDiagnosticsApp::onInput(const InputEvent& event) {
  if (!ctx_) return;
  if (event.action == InputAction::Enter && ctx_->power) {
    ctx_->power->screenOff();
    return;
  }
  last_ = event;
  hasLast_ = true;
}

const char* InputDiagnosticsApp::actionName(InputAction action) const {
  switch (action) {
    case InputAction::Up: return "Up";
    case InputAction::Down: return "Down";
    case InputAction::Left: return "Left";
    case InputAction::Right: return "Right";
    case InputAction::Select: return "Select";
    case InputAction::Back: return "Back";
    case InputAction::Enter: return "Enter";
    case InputAction::TextChar: return "TextChar";
    case InputAction::Backspace: return "Backspace";
    case InputAction::Help: return "Help";
    case InputAction::Wake: return "Wake";
    default: return "None";
  }
}

const char* InputDiagnosticsApp::typeName(InputEventType type) const {
  switch (type) {
    case InputEventType::Press: return "press";
    case InputEventType::Release: return "release";
    case InputEventType::Repeat: return "repeat";
    case InputEventType::LongPress: return "long";
    default: return "?";
  }
}

void SystemInfoApp::draw() {
  if (!ctx_ || !ctx_->ui || !ctx_->storage || !ctx_->network || !ctx_->power) return;
  ctx_->ui->header("System Info");
  ctx_->ui->line(2, String("SD: ") + (ctx_->storage->ready() ? "mounted" : "missing"));
  ctx_->ui->line(3, String("Heap: ") + ESP.getFreeHeap());
  ctx_->ui->line(4, String("Wi-Fi: ") + (ctx_->network->connected() ? "connected" : "disconnected"));
  ctx_->ui->line(5, String("IP: ") + ctx_->network->ip());
  ctx_->ui->line(6, String("Screen: ") + (ctx_->power->displayAwake() ? "on" : "off"));
  ctx_->ui->line(8, "Build: unified-shell pass2", TerminalUI::Green);
  ctx_->ui->line(10, "Features: music rec net web browser ai", TerminalUI::Yellow);
}

void SystemInfoApp::onInput(const InputEvent&) {}
