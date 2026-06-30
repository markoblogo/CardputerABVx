#include <M5Unified.hpp>
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include <driver/sdspi_host.h>
#include <driver/spi_master.h>
#include <esp_random.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <minimp3.h>

#include "assets/abvx_splash.h"
#include "lib/adafruit_tca8418/Adafruit_TCA8418.h"

namespace {
constexpr gpio_num_t PIN_KEYBOARD_INT = GPIO_NUM_11;
constexpr gpio_num_t PIN_SPI_MISO = GPIO_NUM_39;
constexpr gpio_num_t PIN_SPI_MOSI = GPIO_NUM_14;
constexpr gpio_num_t PIN_SPI_SCLK = GPIO_NUM_40;
constexpr gpio_num_t PIN_SD_CS = GPIO_NUM_12;
constexpr const char* MOUNT_POINT = "/sdcard";
constexpr const char* MUSIC_DIR = "/sdcard/music";
constexpr const char* RECORDINGS_DIR = "/sdcard/recordings";
constexpr int SCREEN_W = 240;
constexpr int SCREEN_H = 135;
constexpr int INPUT_BUF_SIZE = 8 * 1024;
constexpr int CHUNK_MS = 120;
constexpr uint32_t DEBOUNCE_MS = 180;
constexpr float PI = 3.14159265358979323846f;

Adafruit_TCA8418 keyboard;
sdmmc_card_t* sd_card = nullptr;
bool spi_ready = false;
bool sd_ready = false;

LGFX_Sprite canvas(&M5.Display);

enum class Screen { Launcher, MusicList, MusicPlaying, RecorderList, RecorderRecording, RecorderPlaying, Message };
enum class Key { None, Up, Down, Left, Right, Ok, Back, Home, One };
enum class VolumeMode { Mute = 0, Mid = 1, Loud = 2 };

struct KeyEvent {
    Key key = Key::None;
    const char* name = "";
};

struct RawKey {
    bool pressed = false;
    uint8_t row = 233;
    uint8_t col = 233;
};

struct KeyCell {
    const char* normal;
    const char* shifted;
    const char* fn;
};

const KeyCell KEYMAP[4][14] = {
    {{"`", "~", "esc"}, {"1", "!", nullptr}, {"2", "@", nullptr}, {"3", "#", nullptr},
     {"4", "$", nullptr}, {"5", "%", nullptr}, {"6", "^", nullptr}, {"7", "&", nullptr},
     {"8", "*", nullptr}, {"9", "(", nullptr}, {"0", ")", nullptr}, {"-", "_", nullptr},
     {"=", "+", nullptr}, {"del", "del", "del"}},
    {{"tab", "tab", nullptr}, {"q", "Q", "Q"}, {"w", "W", "W"}, {"e", "E", "E"},
     {"r", "R", "R"}, {"t", "T", "T"}, {"y", "Y", "Y"}, {"u", "U", "U"},
     {"i", "I", "I"}, {"o", "O", "O"}, {"p", "P", "P"}, {"[", "{", nullptr},
     {"]", "}", nullptr}, {"\\", "|", nullptr}},
    {{"fn", "fn", nullptr}, {"shift", "shift", nullptr}, {"a", "A", "A"}, {"s", "S", "S"},
     {"d", "D", "D"}, {"f", "F", "F"}, {"g", "G", "G"}, {"h", "H", "H"},
     {"j", "J", "J"}, {"k", "K", "K"}, {"l", "L", "L"}, {";", ":", "up"},
     {"'", "\"", nullptr}, {"enter", "enter", nullptr}},
    {{"ctrl", "ctrl", nullptr}, {"opt", "opt", nullptr}, {"alt", "alt", nullptr}, {"z", "Z", "Z"},
     {"x", "X", "X"}, {"c", "C", "C"}, {"v", "V", "V"}, {"b", "B", "B"},
     {"n", "N", "N"}, {"m", "M", "M"}, {",", "<", "left"}, {".", ">", "down"},
     {"/", "?", "right"}, {" ", " ", nullptr}}};

bool shift_down = false;
bool fn_down = false;
std::string last_key_name;
uint32_t last_key_ms = 0;
uint32_t input_block_until_ms = 0;

Screen screen = Screen::Launcher;
int launcher_index = 0;
std::string message_title;
std::string message_body;
bool message_returns_music = false;
uint32_t message_hold_until_ms = 0;

std::vector<std::string> tracks;
std::vector<std::string> recordings;
int selected_track = 0;
int selected_recording = 0;
bool shuffle_on = false;
VolumeMode volume_mode = VolumeMode::Mid;
uint32_t last_input_ms = 0;
bool display_off = false;
bool display_dim = false;
bool dirty = true;

FILE* mp3_file = nullptr;
mp3dec_t mp3_dec;
std::vector<uint8_t> mp3_buf;
size_t mp3_len = 0;
size_t mp3_pos = 0;
bool mp3_eof = false;
bool playing = false;
uint32_t playback_decode_after_ms = 0;
std::vector<int16_t> pcm_chunk;
int pcm_rate = 44100;
int pcm_channels = 2;
int decoded_chunks = 0;

constexpr int REC_SAMPLE_RATE = 16000;
constexpr size_t REC_BUFFER_SAMPLES = 512;
FILE* rec_file = nullptr;
FILE* rec_play_file = nullptr;
std::vector<int16_t> rec_buffer;
uint32_t rec_samples_written = 0;
uint32_t rec_started_ms = 0;
uint32_t rec_play_chunks = 0;
int recorder_cursor = 0;
std::string active_recording_name;

void flushKeyboardEvents()
{
    while (keyboard.available()) {
        keyboard.getEvent();
    }
    keyboard.writeRegister8(TCA8418_REG_INT_STAT, 1);
    (void)M5.BtnA.wasClicked();
    last_key_name.clear();
}

void blockInput(uint32_t ms)
{
    input_block_until_ms = M5.millis() + ms;
    flushKeyboardEvents();
}

RawKey decodeRaw(uint8_t event_raw)
{
    RawKey ret;
    ret.pressed = event_raw & 0x80;
    uint16_t buf = event_raw & 0x7F;
    if (buf == 0) return ret;
    --buf;
    uint8_t row = buf / 10;
    uint8_t col = buf % 10;
    uint8_t mapped_col = row * 2;
    if (col > 3) mapped_col++;
    uint8_t mapped_row = (col + 4) % 4;
    ret.row = mapped_row;
    ret.col = mapped_col;
    return ret;
}

const char* keyNameFromRaw(const RawKey& raw)
{
    if (raw.row > 3 || raw.col > 13) return "";
    if (raw.row == 2 && raw.col == 0) return "fn";
    const auto& cell = KEYMAP[raw.row][raw.col];
    if (fn_down && cell.fn) return cell.fn;
    return shift_down ? cell.shifted : cell.normal;
}

Key keyFromName(const char* name)
{
    if (!name) return Key::None;
    if (!strcmp(name, "up") || !strcmp(name, ":") || !strcmp(name, ";")) return Key::Up;
    if (!strcmp(name, "down") || !strcmp(name, ".")) return Key::Down;
    if (!strcmp(name, "left") || !strcmp(name, ",")) return Key::Left;
    if (!strcmp(name, "right") || !strcmp(name, "/")) return Key::Right;
    if (!strcmp(name, "enter")) return Key::Ok;
    if (!strcmp(name, "'" ) || !strcmp(name, "esc")) return Key::Back;
    if (!strcmp(name, "1")) return Key::One;
    return Key::None;
}

KeyEvent pollKey()
{
    if (M5.millis() < input_block_until_ms) {
        flushKeyboardEvents();
        return {};
    }
    if (M5.BtnA.wasClicked()) return {Key::Home, "GO"};
    if (!keyboard.available()) return {};
    RawKey raw = decodeRaw(keyboard.getEvent());
    keyboard.writeRegister8(TCA8418_REG_INT_STAT, 1);
    if (raw.row == 2 && raw.col == 0) {
        fn_down = raw.pressed;
        return {};
    }
    if (raw.row == 2 && raw.col == 1) {
        shift_down = raw.pressed;
        return {};
    }
    if (!raw.pressed) return {};
    const char* name = keyNameFromRaw(raw);
    if (!strcmp(name, "shift") || !strcmp(name, "fn")) return {};
    const uint32_t now = M5.millis();
    if (last_key_name == name && now - last_key_ms < DEBOUNCE_MS) return {};
    last_key_name = name;
    last_key_ms = now;
    return {keyFromName(name), name};
}

void setBrightnessNormal()
{
    M5.Display.setBrightness(90);
    display_off = false;
    display_dim = false;
}

void drawSplash()
{
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setBrightness(0);
    M5.Display.pushImage(0, 0, 240, 135, image_data_logo);
    for (int b = 0; b <= 100; b += 10) {
        M5.Display.setBrightness(b);
        M5.delay(25);
    }
    M5.delay(650);
    setBrightnessNormal();
}

bool initKeyboard()
{
    if (!keyboard.begin()) return false;
    keyboard.matrix(7, 8);
    keyboard.flush();
    keyboard.enableInterrupts();
    return true;
}

bool initSd()
{
    if (sd_ready) return true;
    esp_err_t ret;
    if (!spi_ready) {
        sdmmc_host_t host = SDSPI_HOST_DEFAULT();
        spi_bus_config_t bus_cfg = {};
        bus_cfg.mosi_io_num = PIN_SPI_MOSI;
        bus_cfg.miso_io_num = PIN_SPI_MISO;
        bus_cfg.sclk_io_num = PIN_SPI_SCLK;
        bus_cfg.quadwp_io_num = -1;
        bus_cfg.quadhd_io_num = -1;
        bus_cfg.max_transfer_sz = 4000;
        ret = spi_bus_initialize((spi_host_device_t)host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return false;
        spi_ready = true;
    }
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    // Cardputer ADV SD was proven stable at 400 kHz in the earlier ultra-safe firmware.
    // Keep the minimal firmware conservative until larger file reads are proven.
    host.max_freq_khz = 400;
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_SD_CS;
    slot_config.host_id = (spi_host_device_t)host.slot;
    ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &sd_card);
    sd_ready = (ret == ESP_OK);
    return sd_ready;
}

bool isHidden(const std::string& name)
{
    return name.empty() || name[0] == '.' || name.rfind("._", 0) == 0 || name == ".DS_Store";
}

std::string lowerExt(const std::string& name)
{
    const auto dot = name.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string ext = name.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return ext;
}

bool hasMp3Ext(const std::string& name)
{
    return lowerExt(name) == ".mp3";
}

size_t findSyncInBytes(const uint8_t* data, size_t len)
{
    for (size_t i = 0; i + 1 < len; ++i) {
        if (data[i] == 0xFF && (data[i + 1] & 0xE0) == 0xE0) return i;
        if ((i & 0x3FF) == 0) {
            vTaskDelay(1);
        }
    }
    return std::string::npos;
}

size_t findSyncInBuffer(const std::vector<uint8_t>& buf, size_t len)
{
    return findSyncInBytes(buf.data(), len);
}

bool hasRecordingExt(const std::string& name)
{
    const std::string ext = lowerExt(name);
    return ext == ".wav" || ext == ".pcm";
}

void writeLe16(FILE* f, uint16_t v)
{
    uint8_t b[2] = {static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>((v >> 8) & 0xFF)};
    fwrite(b, 1, 2, f);
}

void writeLe32(FILE* f, uint32_t v)
{
    uint8_t b[4] = {static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>((v >> 8) & 0xFF),
                    static_cast<uint8_t>((v >> 16) & 0xFF), static_cast<uint8_t>((v >> 24) & 0xFF)};
    fwrite(b, 1, 4, f);
}

void writeWavHeader(FILE* f, uint32_t samples)
{
    const uint32_t data_bytes = samples * sizeof(int16_t);
    const uint32_t byte_rate = REC_SAMPLE_RATE * sizeof(int16_t);
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f);
    writeLe32(f, 36 + data_bytes);
    fwrite("WAVEfmt ", 1, 8, f);
    writeLe32(f, 16);
    writeLe16(f, 1);
    writeLe16(f, 1);
    writeLe32(f, REC_SAMPLE_RATE);
    writeLe32(f, byte_rate);
    writeLe16(f, sizeof(int16_t));
    writeLe16(f, 16);
    fwrite("data", 1, 4, f);
    writeLe32(f, data_bytes);
}

bool ensureRecordingsDir()
{
    if (!initSd()) return false;
    if (mkdir(RECORDINGS_DIR, 0775) != 0 && errno != EEXIST) return false;
    return true;
}

std::string nextRecordingName()
{
    bool used[1000] = {};
    for (const auto& name : recordings) {
        if (name.size() == 11 && name.rfind("REC", 0) == 0 && lowerExt(name) == ".wav") {
            int n = std::atoi(name.substr(3, 4).c_str());
            if (n >= 0 && n < 1000) used[n] = true;
        }
    }
    for (int i = 1; i < 1000; ++i) {
        if (!used[i]) {
            char buf[16];
            snprintf(buf, sizeof(buf), "REC%04d.WAV", i);
            return buf;
        }
    }
    return "REC9999.WAV";
}


void scanMusic()
{
    tracks.clear();
    if (!initSd()) return;
    DIR* dir = opendir(MUSIC_DIR);
    if (!dir) return;
    while (dirent* entry = readdir(dir)) {
        std::string name = entry->d_name;
        if (isHidden(name) || !hasMp3Ext(name)) continue;
        if (entry->d_type == DT_DIR) continue;
        tracks.push_back(name);
    }
    closedir(dir);
    std::sort(tracks.begin(), tracks.end());
    if (selected_track >= static_cast<int>(tracks.size())) selected_track = std::max(0, static_cast<int>(tracks.size()) - 1);
}

void scanRecordings()
{
    recordings.clear();
    if (!ensureRecordingsDir()) return;
    DIR* dir = opendir(RECORDINGS_DIR);
    if (!dir) return;
    while (dirent* entry = readdir(dir)) {
        std::string name = entry->d_name;
        if (isHidden(name) || !hasRecordingExt(name)) continue;
        if (entry->d_type == DT_DIR) continue;
        recordings.push_back(name);
    }
    closedir(dir);
    std::sort(recordings.begin(), recordings.end());
    if (selected_recording >= static_cast<int>(recordings.size())) selected_recording = std::max(0, static_cast<int>(recordings.size()) - 1);
    recorder_cursor = std::min(recorder_cursor, static_cast<int>(recordings.size()));
}

std::string selectedPath()
{
    if (tracks.empty()) return "";
    return std::string(MUSIC_DIR) + "/" + tracks[selected_track];
}

void applyVolume()
{
    int vol = 128;
    if (volume_mode == VolumeMode::Mute) vol = 0;
    if (volume_mode == VolumeMode::Loud) vol = 255;
    M5.Speaker.setVolume(vol);
}

const char* volumeName()
{
    if (volume_mode == VolumeMode::Mute) return "MUTE";
    if (volume_mode == VolumeMode::Loud) return "LOUD";
    return "MID";
}

void stopPlayback()
{
    playing = false;
    M5.Speaker.stop();
    if (mp3_file) {
        fclose(mp3_file);
        mp3_file = nullptr;
    }
    mp3_len = 0;
    mp3_pos = 0;
    mp3_eof = false;
    decoded_chunks = 0;
    playback_decode_after_ms = 0;
}

bool refillInput()
{
    if (!mp3_file) return false;
    if (mp3_pos > 0 && (mp3_pos > mp3_buf.size() / 2 || mp3_len - mp3_pos < 8192)) {
        const size_t remain = mp3_len - mp3_pos;
        memmove(mp3_buf.data(), mp3_buf.data() + mp3_pos, remain);
        mp3_len = remain;
        mp3_pos = 0;
    }
    while (!mp3_eof && mp3_len < mp3_buf.size()) {
        const size_t n = fread(mp3_buf.data() + mp3_len, 1, mp3_buf.size() - mp3_len, mp3_file);
        mp3_len += n;
        if (n == 0) mp3_eof = true;
    }
    return mp3_len > mp3_pos;
}

size_t findSync(size_t start)
{
    for (size_t i = start; i + 1 < mp3_len; ++i) {
        if (mp3_buf[i] == 0xFF && (mp3_buf[i + 1] & 0xE0) == 0xE0) return i;
    }
    return std::string::npos;
}

bool startPlayback(std::string* err = nullptr)
{
    stopPlayback();
    if (tracks.empty()) {
        if (err) *err = "no tracks";
        return false;
    }
    std::string path = selectedPath();
    mp3_file = fopen(path.c_str(), "rb");
    if (!mp3_file) {
        if (err) {
            *err = "open failed: ";
            *err += std::strerror(errno);
        }
        return false;
    }
    mp3dec_init(&mp3_dec);
    mp3_buf.assign(INPUT_BUF_SIZE, 0);
    mp3_len = 0;
    mp3_pos = 0;
    mp3_eof = false;
    pcm_chunk.clear();
    decoded_chunks = 0;
    M5.Mic.end();
    M5.Speaker.begin();
    applyVolume();
    playing = true;
    playback_decode_after_ms = M5.millis() + 800;
    screen = Screen::MusicPlaying;
    dirty = true;
    blockInput(400);
    return true;
}

void nextTrack(int delta)
{
    if (tracks.empty()) return;
    if (shuffle_on && tracks.size() > 1) {
        selected_track = esp_random() % tracks.size();
    } else {
        selected_track = (selected_track + delta + tracks.size()) % tracks.size();
    }
    if (playing) startPlayback();
    dirty = true;
}

void drawWaveform(const std::vector<int16_t>& pcm, int channels)
{
    if (display_off || pcm.empty()) return;
    const int x = 8, y = 76, w = 224, h = 38;
    canvas.fillRect(x, y, w, h, TFT_BLACK);
    canvas.drawRect(x, y, w, h, TFT_DARKGREY);
    const int mid = y + h / 2;
    const size_t frames = pcm.size() / std::max(1, channels);
    if (frames == 0) return;
    int prev_x = x;
    int prev_y = mid;
    for (int px = 0; px < w; ++px) {
        const size_t idx = (static_cast<size_t>(px) * frames / w) * std::max(1, channels);
        int sample = pcm[std::min(idx, pcm.size() - 1)] >> 9;
        int yy = std::max(y + 1, std::min(y + h - 2, mid - sample));
        int xx = x + px;
        if (px > 0) canvas.drawLine(prev_x, prev_y, xx, yy, TFT_WHITE);
        prev_x = xx;
        prev_y = yy;
    }
}

bool decodeChunk(std::string* err = nullptr)
{
    if (!playing || !mp3_file) {
        if (err) *err = "not playing";
        return false;
    }
    pcm_chunk.clear();
    int target_values = 44100 * 2 * CHUNK_MS / 1000;
    std::vector<mp3d_sample_t> frame_pcm(MINIMP3_MAX_SAMPLES_PER_FRAME);
    bool saw_sync = false;
    for (int attempts = 0; attempts < 512 && static_cast<int>(pcm_chunk.size()) < target_values;) {
        if (!refillInput()) break;
        size_t sync = findSync(mp3_pos);
        if (sync == std::string::npos) {
            if (mp3_eof) break;
            mp3_pos = mp3_len;
            continue;
        }
        saw_sync = true;
        mp3_pos = sync;
        mp3dec_frame_info_t info = {};
        int samples = mp3dec_decode_frame(&mp3_dec, mp3_buf.data() + mp3_pos, static_cast<int>(mp3_len - mp3_pos), frame_pcm.data(), &info);
        ++attempts;
        if (samples > 0 && info.frame_bytes > 0 && info.channels > 0) {
            pcm_rate = info.hz;
            pcm_channels = info.channels;
            const size_t values = static_cast<size_t>(samples) * info.channels;
            pcm_chunk.insert(pcm_chunk.end(), frame_pcm.begin(), frame_pcm.begin() + values);
            mp3_pos += info.frame_bytes;
            target_values = std::max(1024, info.hz * info.channels * CHUNK_MS / 1000);
        } else {
            ++mp3_pos;
        }
    }
    if (pcm_chunk.empty()) {
        if (err) {
            if (!saw_sync) *err = mp3_eof ? "no mpeg sync / eof" : "no mpeg sync";
            else *err = "decode produced no pcm";
        }
        return false;
    }
    return true;
}

void updateAudio()
{
    if (!playing) return;
    if (M5.millis() < playback_decode_after_ms) return;
    if (M5.Speaker.isPlaying()) return;
    std::string err;
    if (!decodeChunk(&err)) {
        const bool eof = mp3_eof && err.find("eof") != std::string::npos;
        if (eof) {
            if (tracks.size() > 1) {
                nextTrack(1);
            } else {
                stopPlayback();
                screen = Screen::MusicList;
            }
        } else {
            stopPlayback();
            message_title = "Playback failed";
            message_body = err.empty() ? "decode failed" : err;
            message_returns_music = true;
            screen = Screen::Message;
        }
        dirty = true;
        blockInput(350);
        return;
    }
    ++decoded_chunks;
    if (!display_off) dirty = true;
    M5.Speaker.playRaw(pcm_chunk.data(), pcm_chunk.size(), pcm_rate, pcm_channels == 2, 1, -1, true);
}


bool startRecording(std::string* err = nullptr)
{
    stopPlayback();
    if (!ensureRecordingsDir()) {
        if (err) *err = "no sd/dir";
        return false;
    }
    scanRecordings();
    active_recording_name = nextRecordingName();
    const std::string path = std::string(RECORDINGS_DIR) + "/" + active_recording_name;
    rec_file = fopen(path.c_str(), "wb+");
    if (!rec_file) {
        if (err) {
            *err = "open: ";
            *err += std::strerror(errno);
        }
        return false;
    }
    writeWavHeader(rec_file, 0);
    rec_samples_written = 0;
    rec_started_ms = M5.millis();
    rec_buffer.assign(REC_BUFFER_SAMPLES, 0);

    M5.Speaker.end();
    auto cfg = M5.Mic.config();
    cfg.magnification = 128;
    cfg.noise_filter_level = 2;
    M5.Mic.config(cfg);
    M5.Mic.begin();

    screen = Screen::RecorderRecording;
    dirty = true;
    blockInput(500);
    return true;
}

void stopRecording(bool save)
{
    while (M5.Mic.isRecording()) {
        M5.delay(1);
    }
    M5.Mic.end();
    if (rec_file) {
        if (save) {
            writeWavHeader(rec_file, rec_samples_written);
            fflush(rec_file);
        }
        fclose(rec_file);
        rec_file = nullptr;
    }
    M5.Speaker.begin();
    applyVolume();
    scanRecordings();
    screen = Screen::RecorderList;
    dirty = true;
    blockInput(400);
}

void updateRecording()
{
    if (screen != Screen::RecorderRecording || !rec_file || rec_buffer.empty()) return;
    if (M5.Mic.record(rec_buffer.data(), rec_buffer.size(), REC_SAMPLE_RATE)) {
        fwrite(rec_buffer.data(), sizeof(int16_t), rec_buffer.size(), rec_file);
        rec_samples_written += rec_buffer.size();
        pcm_chunk = rec_buffer;
        pcm_channels = 1;
        pcm_rate = REC_SAMPLE_RATE;
        if (!display_off) dirty = true;
    }
}

std::string selectedRecordingPath()
{
    if (recordings.empty() || recorder_cursor <= 0) return "";
    return std::string(RECORDINGS_DIR) + "/" + recordings[recorder_cursor - 1];
}

bool startRecordingPlayback(std::string* err = nullptr)
{
    stopPlayback();
    if (recorder_cursor <= 0 || recordings.empty()) {
        if (err) *err = "no file";
        return false;
    }
    const std::string path = selectedRecordingPath();
    rec_play_file = fopen(path.c_str(), "rb");
    if (!rec_play_file) {
        if (err) {
            *err = "open: ";
            *err += std::strerror(errno);
        }
        return false;
    }
    fseek(rec_play_file, 44, SEEK_SET);
    rec_buffer.assign(REC_BUFFER_SAMPLES * 4, 0);
    rec_play_chunks = 0;
    active_recording_name = recordings[recorder_cursor - 1];
    M5.Mic.end();
    M5.Speaker.begin();
    applyVolume();
    screen = Screen::RecorderPlaying;
    dirty = true;
    blockInput(500);
    return true;
}

void stopRecordingPlayback()
{
    M5.Speaker.stop();
    if (rec_play_file) {
        fclose(rec_play_file);
        rec_play_file = nullptr;
    }
    screen = Screen::RecorderList;
    dirty = true;
    blockInput(350);
}

void updateRecordingPlayback()
{
    if (screen != Screen::RecorderPlaying || !rec_play_file) return;
    if (M5.Speaker.isPlaying()) return;
    const size_t n = fread(rec_buffer.data(), sizeof(int16_t), rec_buffer.size(), rec_play_file);
    if (n == 0) {
        stopRecordingPlayback();
        return;
    }
    pcm_chunk.assign(rec_buffer.begin(), rec_buffer.begin() + n);
    pcm_channels = 1;
    pcm_rate = REC_SAMPLE_RATE;
    ++rec_play_chunks;
    if (!display_off) dirty = true;
    M5.Speaker.playRaw(rec_buffer.data(), n, REC_SAMPLE_RATE, false, 1, -1, true);
}

void drawLauncher()
{
    static const char* labels[] = {"[#] MUSIC", "[=] READER", "[+] NOTES", "[o] RECORD", "[~] TIME", "[*] TOOLS"};
    canvas.fillScreen(TFT_BLACK);
    canvas.setTextSize(2);
    canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    canvas.setCursor(8, 8);
    canvas.println("ABVx");
    int start = std::max(0, launcher_index - 1);
    start = std::min(start, 3);
    for (int i = start; i < std::min(6, start + 3); ++i) {
        canvas.setCursor(8, 38 + (i - start) * 24);
        canvas.setTextColor(i == launcher_index ? TFT_BLACK : TFT_WHITE, i == launcher_index ? TFT_WHITE : TFT_BLACK);
        canvas.printf("%c %s", i == launcher_index ? '>' : ' ', labels[i]);
    }
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_DARKGREY, TFT_BLACK);
    canvas.setCursor(8, 122);
    canvas.print("OK OPEN   GO MUSIC");
    canvas.pushSprite(0, 0);
}

void drawMusicList()
{
    canvas.fillScreen(TFT_BLACK);
    canvas.setTextSize(2);
    canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    canvas.setCursor(8, 8);
    canvas.printf("MUSIC  %d/%d", tracks.empty() ? 0 : selected_track + 1, static_cast<int>(tracks.size()));
    if (tracks.empty()) {
        canvas.setCursor(8, 42);
        canvas.println(sd_ready ? "No MP3" : "No SD");
        canvas.setTextSize(1);
        canvas.setCursor(8, 76);
        canvas.println("/sdcard/music/A.MP3");
    } else {
        int start = std::max(0, selected_track - 1);
        start = std::min(start, std::max(0, static_cast<int>(tracks.size()) - 3));
        int end = std::min(static_cast<int>(tracks.size()), start + 3);
        for (int i = start; i < end; ++i) {
            canvas.setCursor(8, 38 + (i - start) * 24);
            canvas.setTextColor(i == selected_track ? TFT_BLACK : TFT_WHITE, i == selected_track ? TFT_WHITE : TFT_BLACK);
            canvas.printf("%c %.13s", i == selected_track ? '>' : ' ', tracks[i].c_str());
        }
    }
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_DARKGREY, TFT_BLACK);
    canvas.setCursor(8, 122);
    canvas.printf("OK PLAY  1 SHUF:%s  GO BACK", shuffle_on ? "ON" : "OFF");
    canvas.pushSprite(0, 0);
}

void drawMusicPlaying()
{
    canvas.fillScreen(TFT_BLACK);
    canvas.setTextSize(2);
    canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    canvas.setCursor(8, 8);
    canvas.println("PLAYING");
    canvas.setCursor(8, 34);
    canvas.printf("%.14s", tracks.empty() ? "" : tracks[selected_track].c_str());
    canvas.setCursor(8, 58);
    canvas.printf("V:%s S:%s C:%d", volumeName(), shuffle_on ? "ON" : "OFF", decoded_chunks);
    drawWaveform(pcm_chunk, pcm_channels);
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_DARKGREY, TFT_BLACK);
    canvas.setCursor(8, 122);
    canvas.print("OK/GO STOP  UP/DN VOL  L/R TRACK");
    canvas.pushSprite(0, 0);
}

void drawRecorderList()
{
    canvas.fillScreen(TFT_BLACK);
    canvas.setTextSize(2);
    canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    canvas.setCursor(8, 8);
    canvas.printf("REC %d/%d", recorder_cursor + 1, static_cast<int>(recordings.size()) + 1);

    const int total = static_cast<int>(recordings.size()) + 1;
    int start = std::max(0, recorder_cursor - 1);
    start = std::min(start, std::max(0, total - 3));
    int end = std::min(total, start + 3);
    for (int i = start; i < end; ++i) {
        canvas.setCursor(8, 38 + (i - start) * 24);
        canvas.setTextColor(i == recorder_cursor ? TFT_BLACK : TFT_WHITE, i == recorder_cursor ? TFT_WHITE : TFT_BLACK);
        if (i == 0) {
            canvas.printf("%c NEW REC", i == recorder_cursor ? '>' : ' ');
        } else {
            canvas.printf("%c %.13s", i == recorder_cursor ? '>' : ' ', recordings[i - 1].c_str());
        }
    }
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_DARKGREY, TFT_BLACK);
    canvas.setCursor(8, 122);
    canvas.print("OK REC/PLAY   GO BACK");
    canvas.pushSprite(0, 0);
}

void drawRecorderRecording()
{
    canvas.fillScreen(TFT_BLACK);
    canvas.setTextSize(2);
    canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    canvas.setCursor(8, 8);
    canvas.println("RECORDING");
    canvas.setCursor(8, 34);
    canvas.printf("%.14s", active_recording_name.c_str());
    canvas.setCursor(8, 58);
    canvas.printf("%lus", static_cast<unsigned long>((M5.millis() - rec_started_ms) / 1000));
    drawWaveform(pcm_chunk, 1);
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_DARKGREY, TFT_BLACK);
    canvas.setCursor(8, 122);
    canvas.print("OK SAVE   GO SAVE");
    canvas.pushSprite(0, 0);
}

void drawRecorderPlaying()
{
    canvas.fillScreen(TFT_BLACK);
    canvas.setTextSize(2);
    canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    canvas.setCursor(8, 8);
    canvas.println("REC PLAY");
    canvas.setCursor(8, 34);
    canvas.printf("%.14s", active_recording_name.c_str());
    canvas.setCursor(8, 58);
    canvas.printf("C:%lu", static_cast<unsigned long>(rec_play_chunks));
    drawWaveform(pcm_chunk, 1);
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_DARKGREY, TFT_BLACK);
    canvas.setCursor(8, 122);
    canvas.print("OK/GO STOP");
    canvas.pushSprite(0, 0);
}

void drawMessage()
{
    canvas.fillScreen(TFT_BLACK);
    canvas.setTextSize(2);
    canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    canvas.setCursor(8, 12);
    canvas.println(message_title.c_str());
    canvas.setTextSize(2);
    canvas.setCursor(8, 42);
    canvas.println(message_body.c_str());
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_DARKGREY, TFT_BLACK);
    canvas.setCursor(8, 122);
    canvas.print("GO BACK");
    canvas.pushSprite(0, 0);
}

void drawIfDirty()
{
    if (!dirty || display_off) return;
    if (screen == Screen::Launcher) drawLauncher();
    else if (screen == Screen::MusicList) drawMusicList();
    else if (screen == Screen::MusicPlaying) drawMusicPlaying();
    else if (screen == Screen::RecorderList) drawRecorderList();
    else if (screen == Screen::RecorderRecording) drawRecorderRecording();
    else if (screen == Screen::RecorderPlaying) drawRecorderPlaying();
    else drawMessage();
    dirty = false;
}

void updatePower()
{
    uint32_t idle = M5.millis() - last_input_ms;
    if (playing) {
        if (idle > 30000 && !display_off) {
            M5.Display.setBrightness(0);
            display_off = true;
            display_dim = false;
        } else if (idle > 10000 && !display_dim && !display_off) {
            M5.Display.setBrightness(15);
            display_dim = true;
        }
    } else if (idle > 60000 && !display_off) {
        M5.Display.setBrightness(0);
        display_off = true;
        display_dim = false;
    }
}

void wakeDisplay()
{
    if (display_off || display_dim) {
        setBrightnessNormal();
        dirty = true;
    }
}

void handleKey(KeyEvent ev)
{
    if (ev.key == Key::None) return;
    last_input_ms = M5.millis();
    if (display_off || display_dim) {
        wakeDisplay();
        return;
    }

    if (screen == Screen::Launcher) {
        if (ev.key == Key::Up) launcher_index = std::max(0, launcher_index - 1);
        else if (ev.key == Key::Down) launcher_index = std::min(5, launcher_index + 1);
        else if (ev.key == Key::Home) { launcher_index = 0; scanMusic(); screen = Screen::MusicList; }
        else if (ev.key == Key::Ok) {
            if (launcher_index == 0) { scanMusic(); screen = Screen::MusicList; }
            else if (launcher_index == 3) { scanRecordings(); screen = Screen::RecorderList; }
            else { message_title = "Coming soon"; message_body = "Music now; Recorder v0.2"; message_returns_music = false; screen = Screen::Message; }
        }
        dirty = true;
        return;
    }

    if (screen == Screen::MusicList) {
        if (ev.key == Key::Up && !tracks.empty()) selected_track = std::max(0, selected_track - 1);
        else if (ev.key == Key::Down && !tracks.empty()) selected_track = std::min(static_cast<int>(tracks.size()) - 1, selected_track + 1);
        else if (ev.key == Key::Left) nextTrack(-1);
        else if (ev.key == Key::Right) nextTrack(1);
        else if (ev.key == Key::One) shuffle_on = !shuffle_on;
        else if (ev.key == Key::Ok) {
            if (!tracks.empty()) {
                std::string err;
                if (!startPlayback(&err)) {
                    message_title = "Playback failed";
                    message_body = err.empty() ? "open failed" : err;
                    message_returns_music = true;
                    screen = Screen::Message;
                    blockInput(500);
                }
            }
        } else if (ev.key == Key::Home || ev.key == Key::Back) {
            screen = Screen::Launcher;
            blockInput(250);
        }
        dirty = true;
        return;
    }

    if (screen == Screen::MusicPlaying) {
        if (ev.key == Key::Ok || ev.key == Key::Home || ev.key == Key::Back) {
            stopPlayback();
            screen = Screen::MusicList;
            blockInput(300);
        }
        else if (ev.key == Key::Left) nextTrack(-1);
        else if (ev.key == Key::Right) nextTrack(1);
        else if (ev.key == Key::Up) { volume_mode = static_cast<VolumeMode>(std::min(2, static_cast<int>(volume_mode) + 1)); applyVolume(); }
        else if (ev.key == Key::Down) { volume_mode = static_cast<VolumeMode>(std::max(0, static_cast<int>(volume_mode) - 1)); applyVolume(); }
        else if (ev.key == Key::One) shuffle_on = !shuffle_on;
        dirty = true;
        return;
    }

    if (screen == Screen::RecorderList) {
        const int total = static_cast<int>(recordings.size()) + 1;
        if (ev.key == Key::Up) recorder_cursor = std::max(0, recorder_cursor - 1);
        else if (ev.key == Key::Down) recorder_cursor = std::min(total - 1, recorder_cursor + 1);
        else if (ev.key == Key::Ok) {
            std::string err;
            if (recorder_cursor == 0) {
                if (!startRecording(&err)) {
                    message_title = "Record failed";
                    message_body = err.empty() ? "start" : err;
                    message_returns_music = false;
                    screen = Screen::Message;
                }
            } else {
                if (!startRecordingPlayback(&err)) {
                    message_title = "Play failed";
                    message_body = err.empty() ? "open" : err;
                    message_returns_music = false;
                    screen = Screen::Message;
                }
            }
        }
        else if (ev.key == Key::Home || ev.key == Key::Back) screen = Screen::Launcher;
        dirty = true;
        return;
    }

    if (screen == Screen::RecorderRecording) {
        if (ev.key == Key::Ok || ev.key == Key::Home || ev.key == Key::Back) stopRecording(true);
        dirty = true;
        return;
    }

    if (screen == Screen::RecorderPlaying) {
        if (ev.key == Key::Ok || ev.key == Key::Home || ev.key == Key::Back) stopRecordingPlayback();
        dirty = true;
        return;
    }

    if (screen == Screen::Message) {
        if (M5.millis() < message_hold_until_ms) return;
        if (message_returns_music && (ev.key == Key::Home || ev.key == Key::Back || ev.key == Key::Ok)) {
            message_returns_music = false;
            screen = Screen::MusicList;
        } else if (ev.key == Key::Home || ev.key == Key::Back || ev.key == Key::Ok) {
            screen = Screen::Launcher;
        }
        dirty = true;
    }
}
}  // namespace

extern "C" void app_main(void)
{
    M5.begin();
    M5.Display.setRotation(1);
    M5.Display.setBrightness(0);
    M5.Speaker.begin();
    M5.Speaker.setVolume(128);
    canvas.createSprite(SCREEN_W, SCREEN_H);
    canvas.setColorDepth(8);

    initKeyboard();
    drawSplash();
    last_input_ms = M5.millis();
    dirty = true;

    while (true) {
        M5.update();
        KeyEvent ev = pollKey();
        handleKey(ev);
        drawIfDirty();
        updateAudio();
        updateRecording();
        updateRecordingPlayback();
        updatePower();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
