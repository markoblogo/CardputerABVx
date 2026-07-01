#include <M5Unified.hpp>
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <map>
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
constexpr const char* BOOKS_DIR = "/sdcard/books";
constexpr const char* NOTES_DIR = "/sdcard/notes";
constexpr const char* RECORDINGS_DIR = "/sdcard/rec";
constexpr int SCREEN_W = 240;
constexpr int SCREEN_H = 135;
constexpr int INPUT_BUF_SIZE = 8 * 1024;
constexpr int CHUNK_MS = 120;
constexpr size_t MAX_BOOK_BYTES = 128 * 1024;
constexpr int READER_LINES_PER_PAGE = 4;
constexpr int SPEED_WPM_MIN = 350;
constexpr int SPEED_WPM_MAX = 1000;
constexpr int SPEED_WPM_STEP = 50;
constexpr uint32_t DEBOUNCE_MS = 180;
constexpr float PI = 3.14159265358979323846f;

Adafruit_TCA8418 keyboard;
sdmmc_card_t* sd_card = nullptr;
bool spi_ready = false;
bool sd_ready = false;

LGFX_Sprite canvas(&M5.Display);

enum class Screen { Launcher, MusicList, MusicPlaying, ReaderList, ReaderView, ReaderSpeed, NotesList, NotesView, NotesEdit, RecorderList, RecorderRecording, RecorderPlaying, TimeApp, FilesList, Message };
enum class Key { None, Up, Down, Left, Right, Ok, Back, Home, One, Backspace };
enum class VolumeMode { Mute = 0, Mid = 1, Loud = 2 };
enum class SpeedMode { OneWord = 0, TwoWords = 1, Line = 2 };
enum class TimeMode { Clock = 0, Stopwatch = 1, Timer = 2, Alarm = 3 };
enum class TimeSetField { Hours = 0, Minutes = 1, Seconds = 2 };

struct KeyEvent {
    Key key = Key::None;
    const char* name = "";
};

struct FileEntry {
    std::string name;
    std::string path;
    bool is_dir = false;
    size_t size = 0;
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
bool message_returns_notes = false;
uint32_t message_hold_until_ms = 0;

std::vector<std::string> tracks;
std::vector<std::string> books;
std::vector<std::string> notes;
std::vector<std::string> recordings;
std::vector<FileEntry> file_entries;
std::string files_path = MOUNT_POINT;
int files_cursor = 0;
int selected_track = 0;
std::string override_music_path;
int selected_book = 0;
int notes_cursor = 0;
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
std::string active_book_name;
std::string active_note_name;
std::string note_input;
bool note_ru_mode = false;
std::string reader_text;
std::vector<std::string> reader_lines;
std::vector<std::string> reader_words;
std::map<std::string, int> reader_bookmarks;
int reader_scroll = 0;
int speed_index = 0;
int speed_wpm = SPEED_WPM_MIN;
SpeedMode speed_mode = SpeedMode::OneWord;
bool speed_paused = true;
uint32_t speed_next_ms = 0;
TimeMode time_mode = TimeMode::Clock;
int clock_seconds = 0;
uint32_t clock_base_ms = 0;
bool stopwatch_running = false;
uint32_t stopwatch_started_ms = 0;
uint32_t stopwatch_elapsed_ms = 0;
int timer_seconds = 300;
uint32_t timer_remaining_ms = 300000;
uint32_t timer_started_ms = 0;
bool timer_running = false;
bool timer_done = false;
TimeSetField time_set_field = TimeSetField::Minutes;
int alarm_seconds = 7 * 3600;
bool alarm_enabled = false;
bool alarm_ringing = false;
uint32_t last_alarm_day = 999999;
uint32_t alert_until_ms = 0;
uint32_t last_alert_beep_ms = 0;

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
    if (!strcmp(name, "del")) return Key::Backspace;
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

bool hasTextExt(const std::string& name)
{
    return lowerExt(name) == ".txt";
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


std::string baseName(const std::string& path)
{
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool isKnownFileExt(const std::string& name)
{
    return hasMp3Ext(name) || hasTextExt(name) || hasRecordingExt(name);
}

void scanFiles(const std::string& path)
{
    file_entries.clear();
    files_path = path.empty() ? std::string(MOUNT_POINT) : path;
    files_cursor = 0;
    if (!initSd()) return;
    DIR* dir = opendir(files_path.c_str());
    if (!dir) return;
    if (files_path != MOUNT_POINT) {
        file_entries.push_back({"..", "", true, 0});
    }
    while (dirent* entry = readdir(dir)) {
        std::string name = entry->d_name;
        if (isHidden(name)) continue;
        std::string full = files_path + "/" + name;
        struct stat st = {};
        if (stat(full.c_str(), &st) != 0) continue;
        bool dir_flag = S_ISDIR(st.st_mode) || entry->d_type == DT_DIR;
        if (!dir_flag && !isKnownFileExt(name)) continue;
        file_entries.push_back({name, full, dir_flag, static_cast<size_t>(st.st_size)});
    }
    closedir(dir);
    std::sort(file_entries.begin() + (files_path == MOUNT_POINT ? 0 : 1), file_entries.end(), [](const FileEntry& a, const FileEntry& b) {
        if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
        return a.name < b.name;
    });
}

std::string parentPath(const std::string& path)
{
    if (path == MOUNT_POINT) return MOUNT_POINT;
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos || slash <= std::strlen(MOUNT_POINT)) return MOUNT_POINT;
    return path.substr(0, slash);
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

bool ensureRecordingsDir(std::string* err = nullptr)
{
    if (!initSd()) {
        if (err) *err = "sd mount";
        return false;
    }
    errno = 0;
    if (mkdir(RECORDINGS_DIR, 0775) != 0 && errno != EEXIST) {
        if (err) {
            *err = "mkdir ";
            *err += std::to_string(errno);
            *err += " ";
            *err += std::strerror(errno);
        }
        return false;
    }
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


void scanBooks()
{
    books.clear();
    if (!initSd()) return;
    DIR* dir = opendir(BOOKS_DIR);
    if (!dir) return;
    while (dirent* entry = readdir(dir)) {
        std::string name = entry->d_name;
        if (isHidden(name) || !hasTextExt(name)) continue;
        if (entry->d_type == DT_DIR) continue;
        books.push_back(name);
    }
    closedir(dir);
    std::sort(books.begin(), books.end());
    if (selected_book >= static_cast<int>(books.size())) selected_book = std::max(0, static_cast<int>(books.size()) - 1);
}

std::string selectedBookPath()
{
    if (books.empty()) return "";
    return std::string(BOOKS_DIR) + "/" + books[selected_book];
}


bool ensureNotesDir(std::string* err = nullptr)
{
    if (!initSd()) {
        if (err) *err = "sd mount";
        return false;
    }
    errno = 0;
    if (mkdir(NOTES_DIR, 0775) != 0 && errno != EEXIST) {
        if (err) {
            *err = "mkdir ";
            *err += std::to_string(errno);
            *err += " ";
            *err += std::strerror(errno);
        }
        return false;
    }
    return true;
}

void scanNotes()
{
    notes.clear();
    if (!ensureNotesDir()) return;
    DIR* dir = opendir(NOTES_DIR);
    if (!dir) return;
    while (dirent* entry = readdir(dir)) {
        std::string name = entry->d_name;
        if (isHidden(name) || !hasTextExt(name)) continue;
        if (entry->d_type == DT_DIR) continue;
        notes.push_back(name);
    }
    closedir(dir);
    std::sort(notes.begin(), notes.end());
    const int total = static_cast<int>(notes.size()) + 1;
    notes_cursor = std::max(0, std::min(notes_cursor, total - 1));
}

std::string selectedNotePath()
{
    if (notes_cursor <= 0 || notes.empty()) return "";
    return std::string(NOTES_DIR) + "/" + notes[notes_cursor - 1];
}

std::string nextNoteName()
{
    bool used[10000] = {};
    for (const auto& name : notes) {
        if (name.size() == 12 && name.rfind("NOTE", 0) == 0 && lowerExt(name) == ".txt") {
            int n = std::atoi(name.substr(4, 4).c_str());
            if (n >= 0 && n < 10000) used[n] = true;
        }
    }
    for (int i = 1; i < 10000; ++i) {
        if (!used[i]) {
            char buf[16];
            snprintf(buf, sizeof(buf), "NOTE%04d.TXT", i);
            return buf;
        }
    }
    return "NOTE9999.TXT";
}

size_t utf8CharLen(unsigned char c)
{
    if ((c & 0x80) == 0) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

void appendWrappedLine(std::vector<std::string>& out, std::string line)
{
    while (!line.empty() && (line.back() == ' ' || line.back() == '\r' || line.back() == '\t')) line.pop_back();
    out.push_back(line.empty() ? " " : line);
}

void wrapReaderText()
{
    reader_lines.clear();
    reader_words.clear();
    std::string line;
    std::string word;
    int line_cols = 0;
    int word_cols = 0;
    constexpr int max_cols = 19;
    auto flush_word = [&]() {
        if (word.empty()) return;
        if (line_cols > 0 && line_cols + 1 + word_cols > max_cols) {
            appendWrappedLine(reader_lines, line);
            line.clear();
            line_cols = 0;
        }
        if (word_cols > max_cols) {
            if (!line.empty()) {
                appendWrappedLine(reader_lines, line);
                line.clear();
                line_cols = 0;
            }
            std::string part;
            int cols = 0;
            for (size_t i = 0; i < word.size();) {
                size_t len = utf8CharLen(static_cast<unsigned char>(word[i]));
                if (i + len > word.size()) len = 1;
                if (cols >= max_cols) {
                    appendWrappedLine(reader_lines, part);
                    part.clear();
                    cols = 0;
                }
                part.append(word, i, len);
                ++cols;
                i += len;
            }
            line = part;
            line_cols = cols;
        } else {
            if (line_cols > 0) {
                line += ' ';
                ++line_cols;
            }
            line += word;
            line_cols += word_cols;
        }
        reader_words.push_back(word);
        word.clear();
        word_cols = 0;
    };

    for (size_t i = 0; i < reader_text.size();) {
        unsigned char c = static_cast<unsigned char>(reader_text[i]);
        if (c == '\r') { ++i; continue; }
        if (c == '\n') {
            flush_word();
            appendWrappedLine(reader_lines, line);
            line.clear();
            line_cols = 0;
            ++i;
            continue;
        }
        if (c == ' ' || c == '\t') {
            flush_word();
            ++i;
            continue;
        }
        size_t len = utf8CharLen(c);
        if (i + len > reader_text.size()) len = 1;
        word.append(reader_text, i, len);
        ++word_cols;
        i += len;
    }
    flush_word();
    if (!line.empty()) appendWrappedLine(reader_lines, line);
    if (reader_lines.empty()) reader_lines.push_back(" ");
}

bool loadTextFile(const std::string& path, const std::string& label, std::string* err = nullptr)
{
    struct stat st = {};
    if (stat(path.c_str(), &st) != 0) {
        if (err) {
            *err = "stat: ";
            *err += std::strerror(errno);
        }
        return false;
    }
    if (st.st_size <= 0) {
        if (err) *err = "empty file";
        return false;
    }
    if (static_cast<size_t>(st.st_size) > MAX_BOOK_BYTES) {
        if (err) *err = "file too big";
        return false;
    }
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        if (err) {
            *err = "open: ";
            *err += std::strerror(errno);
        }
        return false;
    }
    reader_text.assign(static_cast<size_t>(st.st_size), '\0');
    size_t n = fread(reader_text.data(), 1, reader_text.size(), f);
    fclose(f);
    reader_text.resize(n);
    active_book_name = label;
    reader_scroll = 0;
    speed_index = 0;
    speed_wpm = SPEED_WPM_MIN;
    speed_mode = SpeedMode::OneWord;
    speed_paused = true;
    speed_next_ms = 0;
    wrapReaderText();
    return true;
}

int lineIndexForWord(int word_idx);

int clampReaderLine(int line)
{
    int max_scroll = std::max(0, static_cast<int>(reader_lines.size()) - READER_LINES_PER_PAGE);
    return std::max(0, std::min(line, max_scroll));
}

int currentReaderLineForBookmark()
{
    if (screen == Screen::ReaderSpeed) {
        return speed_mode == SpeedMode::Line ? speed_index : lineIndexForWord(speed_index);
    }
    return reader_scroll;
}

void saveReaderBookmark()
{
    if (active_book_name.empty() || reader_lines.empty()) return;
    reader_bookmarks[active_book_name] = clampReaderLine(currentReaderLineForBookmark());
}

bool loadSelectedBook(std::string* err = nullptr)
{
    if (books.empty()) {
        if (err) *err = "no books";
        return false;
    }
    if (!loadTextFile(selectedBookPath(), books[selected_book], err)) return false;
    auto it = reader_bookmarks.find(active_book_name);
    if (it != reader_bookmarks.end()) reader_scroll = clampReaderLine(it->second);
    return true;
}

bool loadSelectedNote(std::string* err = nullptr)
{
    if (notes_cursor <= 0 || notes.empty()) {
        if (err) *err = "no note";
        return false;
    }
    active_note_name = notes[notes_cursor - 1];
    return loadTextFile(selectedNotePath(), active_note_name, err);
}

bool consumeTranslit(const std::string& src, size_t pos, const char* key)
{
    size_t n = std::strlen(key);
    if (pos + n > src.size()) return false;
    for (size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(src[pos + i])) != key[i]) return false;
    }
    return true;
}

std::string translitToRussian(const std::string& src)
{
    std::string out;
    for (size_t i = 0; i < src.size();) {
        unsigned char c = static_cast<unsigned char>(src[i]);
        if (c < 128 && std::isalpha(c)) {
            if (consumeTranslit(src, i, "etot")) { out += "этот"; i += 4; continue; }
            if (consumeTranslit(src, i, "eto")) { out += "это"; i += 3; continue; }
            if (consumeTranslit(src, i, "eta")) { out += "эта"; i += 3; continue; }
            if (consumeTranslit(src, i, "eti")) { out += "эти"; i += 3; continue; }
            if (consumeTranslit(src, i, "shch")) { out += "щ"; i += 4; continue; }
            if (consumeTranslit(src, i, "yo")) { out += "ё"; i += 2; continue; }
            if (consumeTranslit(src, i, "yu")) { out += "ю"; i += 2; continue; }
            if (consumeTranslit(src, i, "ya")) { out += "я"; i += 2; continue; }
            if (consumeTranslit(src, i, "ye")) { out += "е"; i += 2; continue; }
            if (consumeTranslit(src, i, "zh")) { out += "ж"; i += 2; continue; }
            if (consumeTranslit(src, i, "ch")) { out += "ч"; i += 2; continue; }
            if (consumeTranslit(src, i, "sh")) { out += "ш"; i += 2; continue; }
            if (consumeTranslit(src, i, "kh")) { out += "х"; i += 2; continue; }
            if (consumeTranslit(src, i, "ts")) { out += "ц"; i += 2; continue; }
            switch (std::tolower(c)) {
                case 'a': out += "а"; break;
                case 'b': out += "б"; break;
                case 'v': out += "в"; break;
                case 'g': out += "г"; break;
                case 'd': out += "д"; break;
                case 'e': out += "е"; break;
                case 'z': out += "з"; break;
                case 'i': out += "и"; break;
                case 'j': out += "й"; break;
                case 'k': out += "к"; break;
                case 'l': out += "л"; break;
                case 'm': out += "м"; break;
                case 'n': out += "н"; break;
                case 'o': out += "о"; break;
                case 'p': out += "п"; break;
                case 'r': out += "р"; break;
                case 's': out += "с"; break;
                case 't': out += "т"; break;
                case 'u': out += "у"; break;
                case 'f': out += "ф"; break;
                case 'h': out += "х"; break;
                case 'c': out += "ц"; break;
                case 'y': out += "ы"; break;
                case 'x': out += "кс"; break;
                case 'q': out += "к"; break;
                case 'w': out += "в"; break;
                default: out.push_back(src[i]); break;
            }
            ++i;
        } else {
            out.push_back(src[i++]);
        }
    }
    return out;
}

std::string noteTextForSave()
{
    return note_ru_mode ? translitToRussian(note_input) : note_input;
}

std::string utf8TailByChars(const std::string& text, int max_chars)
{
    int chars = 0;
    size_t start = text.size();
    while (start > 0 && chars < max_chars) {
        size_t p = start - 1;
        while (p > 0 && (static_cast<unsigned char>(text[p]) & 0xC0) == 0x80) --p;
        start = p;
        ++chars;
    }
    return text.substr(start);
}

bool saveNewNote(std::string* out_name, std::string* err = nullptr)
{
    if (!ensureNotesDir(err)) return false;
    scanNotes();
    std::string name = nextNoteName();
    std::string path = std::string(NOTES_DIR) + "/" + name;
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        if (err) {
            *err = "open: ";
            *err += std::strerror(errno);
        }
        return false;
    }
    std::string body = noteTextForSave();
    body += "\n";
    size_t n = fwrite(body.data(), 1, body.size(), f);
    bool ok = n == body.size();
    if (fflush(f) != 0) ok = false;
    fclose(f);
    if (!ok) {
        if (err) *err = "write failed";
        return false;
    }
    if (out_name) *out_name = name;
    scanNotes();
    for (int i = 0; i < static_cast<int>(notes.size()); ++i) {
        if (notes[i] == name) {
            notes_cursor = i + 1;
            break;
        }
    }
    return true;
}

int utf8Columns(const std::string& text)
{
    int cols = 0;
    for (size_t i = 0; i < text.size();) {
        size_t len = utf8CharLen(static_cast<unsigned char>(text[i]));
        if (i + len > text.size()) len = 1;
        ++cols;
        i += len;
    }
    return cols;
}

int countWordsInLine(const std::string& line)
{
    int words = 0;
    bool in_word = false;
    for (unsigned char c : line) {
        bool ws = c == ' ' || c == '\t' || c == '\r' || c == '\n';
        if (!ws && !in_word) {
            ++words;
            in_word = true;
        }
        if (ws) in_word = false;
    }
    return std::max(1, words);
}

int wordIndexForLine(int line_idx)
{
    int idx = 0;
    int limit = std::max(0, std::min(line_idx, static_cast<int>(reader_lines.size())));
    for (int i = 0; i < limit; ++i) idx += countWordsInLine(reader_lines[i]);
    return std::min(idx, std::max(0, static_cast<int>(reader_words.size()) - 1));
}

int lineIndexForWord(int word_idx)
{
    int seen = 0;
    for (int i = 0; i < static_cast<int>(reader_lines.size()); ++i) {
        seen += countWordsInLine(reader_lines[i]);
        if (word_idx < seen) return i;
    }
    return std::max(0, static_cast<int>(reader_lines.size()) - 1);
}

void setSpeedMode(SpeedMode next)
{
    if (next == speed_mode) return;
    if (speed_mode == SpeedMode::Line && next != SpeedMode::Line) {
        speed_index = wordIndexForLine(speed_index);
    } else if (speed_mode != SpeedMode::Line && next == SpeedMode::Line) {
        speed_index = lineIndexForWord(speed_index);
    }
    speed_mode = next;
    int max_idx = speed_mode == SpeedMode::Line ? static_cast<int>(reader_lines.size()) - 1 : static_cast<int>(reader_words.size()) - 1;
    speed_index = std::max(0, std::min(speed_index, std::max(0, max_idx)));
}

int speedStepWords()
{
    return speed_mode == SpeedMode::TwoWords ? 2 : 1;
}

uint32_t speedIntervalMs()
{
    if (speed_mode == SpeedMode::Line) {
        int words = 1;
        if (speed_index >= 0 && speed_index < static_cast<int>(reader_lines.size())) {
            words = countWordsInLine(reader_lines[speed_index]);
        }
        return std::max<uint32_t>(120, static_cast<uint32_t>(60000UL * words / speed_wpm));
    }
    return std::max<uint32_t>(120, static_cast<uint32_t>(60000UL * speedStepWords() / speed_wpm));
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
    std::string path = override_music_path.empty() ? selectedPath() : override_music_path;
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
    if (!ensureRecordingsDir(err)) {
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
    static const char* labels[] = {"[#] MUSIC", "[=] READER", "[+] NOTES", "[o] RECORD", "[~] TIME", "[*] FILES"};
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
    canvas.printf("%.14s", !override_music_path.empty() ? baseName(override_music_path).c_str() : (tracks.empty() ? "" : tracks[selected_track].c_str()));
    canvas.setCursor(8, 58);
    canvas.printf("V:%s S:%s C:%d", volumeName(), shuffle_on ? "ON" : "OFF", decoded_chunks);
    drawWaveform(pcm_chunk, pcm_channels);
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_DARKGREY, TFT_BLACK);
    canvas.setCursor(8, 122);
    canvas.print("OK/GO STOP  UP/DN VOL  L/R TRACK");
    canvas.pushSprite(0, 0);
}

void drawNotesList()
{
    canvas.fillScreen(TFT_BLACK);
    canvas.setTextSize(2);
    canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    canvas.setCursor(8, 8);
    canvas.printf("NOTES %d/%d", notes_cursor + 1, static_cast<int>(notes.size()) + 1);
    const int total = static_cast<int>(notes.size()) + 1;
    int rows = 4;
    int start = std::max(0, notes_cursor - 1);
    start = std::min(start, std::max(0, total - rows));
    int end = std::min(total, start + rows);
    for (int i = start; i < end; ++i) {
        canvas.setCursor(8, 34 + (i - start) * 21);
        canvas.setTextColor(i == notes_cursor ? TFT_BLACK : TFT_WHITE, i == notes_cursor ? TFT_WHITE : TFT_BLACK);
        if (i == 0) canvas.printf("%c NEW NOTE", i == notes_cursor ? '>' : ' ');
        else canvas.printf("%c %.13s", i == notes_cursor ? '>' : ' ', notes[i - 1].c_str());
    }
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_DARKGREY, TFT_BLACK);
    canvas.setCursor(8, 122);
    canvas.print("OK NEW/OPEN   GO BACK");
    canvas.pushSprite(0, 0);
}


uint32_t nextUtf8Codepoint(const std::string& text, size_t& i)
{
    unsigned char c = static_cast<unsigned char>(text[i]);
    if (c < 0x80) return text[i++];
    if ((c & 0xE0) == 0xC0 && i + 1 < text.size()) {
        uint32_t cp = ((c & 0x1F) << 6) | (static_cast<unsigned char>(text[i + 1]) & 0x3F);
        i += 2;
        return cp;
    }
    if ((c & 0xF0) == 0xE0 && i + 2 < text.size()) {
        uint32_t cp = ((c & 0x0F) << 12) | ((static_cast<unsigned char>(text[i + 1]) & 0x3F) << 6) |
                      (static_cast<unsigned char>(text[i + 2]) & 0x3F);
        i += 3;
        return cp;
    }
    ++i;
    return '?';
}

bool isCyrillicCodepoint(uint32_t cp)
{
    return (cp >= 0x0400 && cp <= 0x04FF);
}

bool containsCyrillic(const std::string& text)
{
    for (size_t i = 0; i < text.size();) {
        if (isCyrillicCodepoint(nextUtf8Codepoint(text, i))) return true;
    }
    return false;
}

const char** cyrillicBitmap(uint32_t cp)
{
    static const char* A[]  = {"01110","10001","10001","11111","10001","10001","10001"};
    static const char* Be[] = {"11111","10000","10000","11110","10001","10001","11110"};
    static const char* Ve[] = {"11110","10001","10001","11110","10001","10001","11110"};
    static const char* Ge[] = {"11111","10000","10000","10000","10000","10000","10000"};
    static const char* De[] = {"01110","01010","01010","01010","01010","11111","10001"};
    static const char* E[]  = {"11111","10000","10000","11110","10000","10000","11111"};
    static const char* Zh[] = {"10101","10101","01110","00100","01110","10101","10101"};
    static const char* Ze[] = {"11110","00001","00001","01110","00001","00001","11110"};
    static const char* I[]  = {"10001","10011","10101","10101","11001","10001","10001"};
    static const char* K[]  = {"10001","10010","10100","11000","10100","10010","10001"};
    static const char* L[]  = {"00111","01001","01001","01001","01001","10001","10001"};
    static const char* M[]  = {"10001","11011","10101","10101","10001","10001","10001"};
    static const char* N[]  = {"10001","10001","10001","11111","10001","10001","10001"};
    static const char* O[]  = {"01110","10001","10001","10001","10001","10001","01110"};
    static const char* P[]  = {"11111","10001","10001","10001","10001","10001","10001"};
    static const char* R[]  = {"11110","10001","10001","11110","10000","10000","10000"};
    static const char* S[]  = {"01111","10000","10000","10000","10000","10000","01111"};
    static const char* T[]  = {"11111","00100","00100","00100","00100","00100","00100"};
    static const char* U[]  = {"10001","10001","10001","01111","00001","10001","01110"};
    static const char* F[]  = {"00100","01110","10101","10101","01110","00100","00100"};
    static const char* H[]  = {"10001","10001","01010","00100","01010","10001","10001"};
    static const char* Ts[]= {"10010","10010","10010","10010","10010","11111","00001"};
    static const char* Ch[]= {"10001","10001","10001","01111","00001","00001","00001"};
    static const char* Sh[]= {"10101","10101","10101","10101","10101","10101","11111"};
    static const char* Sch[]={"10101","10101","10101","10101","10101","11111","00001"};
    static const char* Y[]  = {"10001","10001","10001","11101","10011","10011","11101"};
    static const char* Soft[]={"10000","10000","10000","11110","10001","10001","11110"};
    static const char* Ee[] = {"11110","00001","00001","01111","00001","00001","11110"};
    static const char* Yu[]= {"10010","10101","10101","11101","10101","10101","10010"};
    static const char* Ya[]= {"01111","10001","10001","01111","00101","01001","10001"};
    static const char* Ii[]= {"00100","00000","01100","00100","00100","00100","01110"};
    static const char* Ie[]= {"01111","10000","10000","11110","10000","10000","01111"};
    switch (cp) {
        case 0x0410: case 0x0430: return A;
        case 0x0411: case 0x0431: return Be;
        case 0x0412: case 0x0432: return Ve;
        case 0x0413: case 0x0433: case 0x0490: case 0x0491: return Ge;
        case 0x0414: case 0x0434: return De;
        case 0x0415: case 0x0435: case 0x0401: case 0x0451: return E;
        case 0x0416: case 0x0436: return Zh;
        case 0x0417: case 0x0437: return Ze;
        case 0x0418: case 0x0438: case 0x0419: case 0x0439: return I;
        case 0x041A: case 0x043A: return K;
        case 0x041B: case 0x043B: return L;
        case 0x041C: case 0x043C: return M;
        case 0x041D: case 0x043D: return N;
        case 0x041E: case 0x043E: return O;
        case 0x041F: case 0x043F: return P;
        case 0x0420: case 0x0440: return R;
        case 0x0421: case 0x0441: return S;
        case 0x0422: case 0x0442: return T;
        case 0x0423: case 0x0443: return U;
        case 0x0424: case 0x0444: return F;
        case 0x0425: case 0x0445: return H;
        case 0x0426: case 0x0446: return Ts;
        case 0x0427: case 0x0447: return Ch;
        case 0x0428: case 0x0448: return Sh;
        case 0x0429: case 0x0449: return Sch;
        case 0x042A: case 0x044A: case 0x042C: case 0x044C: return Soft;
        case 0x042B: case 0x044B: return Y;
        case 0x042D: case 0x044D: return Ee;
        case 0x042E: case 0x044E: return Yu;
        case 0x042F: case 0x044F: return Ya;
        case 0x0406: case 0x0456: case 0x0407: case 0x0457: return Ii;
        case 0x0404: case 0x0454: return Ie;
        default: return nullptr;
    }
}

void drawBitmapGlyph(int x, int y, int scale, const char** rows, uint16_t color)
{
    int dot = std::max(1, scale - 1);
    for (int yy = 0; yy < 7; ++yy) {
        for (int xx = 0; xx < 5; ++xx) {
            if (rows[yy][xx] == '1') canvas.fillRect(x + xx * scale, y + yy * scale, dot, dot, color);
        }
    }
}

void drawMixedTextLine(int x, int y, const std::string& text, int scale = 2)
{
    int cx = x;
    canvas.setTextSize(scale);
    canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    for (size_t i = 0; i < text.size();) {
        size_t before = i;
        uint32_t cp = nextUtf8Codepoint(text, i);
        if (cp == ' ') { cx += 6 * scale; continue; }
        const char** glyph = cyrillicBitmap(cp);
        if (glyph) {
            drawBitmapGlyph(cx, y + 2, scale, glyph, TFT_WHITE);
            cx += 6 * scale;
        } else if (cp < 128) {
            char b[2] = {static_cast<char>(cp), 0};
            canvas.setCursor(cx, y);
            canvas.print(b);
            cx += 6 * scale;
        } else {
            canvas.drawRect(cx, y + 2, 4 * scale, 7 * scale, TFT_WHITE);
            cx += 6 * scale;
        }
        if (i == before) ++i;
        if (cx > SCREEN_W - 8) break;
    }
}

void drawTextLineSmart(int x, int y, const std::string& text)
{
    if (containsCyrillic(text)) {
        drawMixedTextLine(x, y, text, 2);
    } else {
        canvas.print(text.c_str());
    }
}

void drawNotesView()
{
    canvas.fillScreen(TFT_BLACK);
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_DARKGREY, TFT_BLACK);
    canvas.setCursor(8, 5);
    canvas.printf("%.14s %d/%d", active_note_name.c_str(), reader_lines.empty() ? 0 : reader_scroll + 1, static_cast<int>(reader_lines.size()));
    canvas.setTextSize(2);
    canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    for (int row = 0; row < READER_LINES_PER_PAGE; ++row) {
        int idx = reader_scroll + row;
        if (idx >= static_cast<int>(reader_lines.size())) break;
        canvas.setCursor(8, 22 + row * 24);
        drawTextLineSmart(8, 22 + row * 24, reader_lines[idx]);
    }
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_DARKGREY, TFT_BLACK);
    canvas.setCursor(8, 122);
    canvas.print("UP/DN LINE  L/R PAGE  GO LIST");
    canvas.pushSprite(0, 0);
}

void drawNotesEdit()
{
    canvas.fillScreen(TFT_BLACK);
    canvas.setTextSize(2);
    canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    canvas.setCursor(8, 8);
    canvas.println("NEW NOTE");
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_DARKGREY, TFT_BLACK);
    canvas.setCursor(8, 34);
    canvas.printf("%s  1 toggle", note_ru_mode ? "RU translit" : "LAT text");
    canvas.setTextSize(2);
    canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    std::string display_text = note_input;
    std::string tail = utf8TailByChars(display_text, 57);
    for (int row = 0; row < 3; ++row) {
        canvas.setCursor(8, 52 + row * 22);
        size_t off = row * 19;
        if (off < tail.size()) canvas.print(tail.substr(off, 19).c_str());
    }
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_DARKGREY, TFT_BLACK);
    canvas.setCursor(8, 112);
    canvas.printf("%s %d/512", note_ru_mode ? "RU SAVE" : "LAT", static_cast<int>(note_input.size()));
    canvas.setCursor(8, 122);
    canvas.print("OK SAVE  1 LAT/RU  GO CANCEL");
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

void drawReaderList()
{
    canvas.fillScreen(TFT_BLACK);
    canvas.setTextSize(2);
    canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    canvas.setCursor(8, 8);
    canvas.printf("BOOKS %d/%d", books.empty() ? 0 : selected_book + 1, static_cast<int>(books.size()));
    if (books.empty()) {
        canvas.setCursor(8, 42);
        canvas.println(sd_ready ? "No books" : "No SD");
        canvas.setTextSize(1);
        canvas.setCursor(8, 76);
        canvas.println("/sdcard/books/EN1.TXT");
        canvas.setCursor(8, 90);
        canvas.println("/sdcard/books/RU1.TXT");
    } else {
        int rows = 4;
        int start = std::max(0, selected_book - 1);
        start = std::min(start, std::max(0, static_cast<int>(books.size()) - rows));
        int end = std::min(static_cast<int>(books.size()), start + rows);
        for (int i = start; i < end; ++i) {
            canvas.setCursor(8, 34 + (i - start) * 21);
            canvas.setTextColor(i == selected_book ? TFT_BLACK : TFT_WHITE, i == selected_book ? TFT_WHITE : TFT_BLACK);
            canvas.printf("%c%c%.12s", i == selected_book ? '>' : ' ', reader_bookmarks.count(books[i]) ? '*' : ' ', books[i].c_str());
        }
    }
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_DARKGREY, TFT_BLACK);
    canvas.setCursor(8, 122);
    canvas.print("OK READ   GO BACK");
    canvas.pushSprite(0, 0);
}

void drawReaderView()
{
    canvas.fillScreen(TFT_BLACK);
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_DARKGREY, TFT_BLACK);
    canvas.setCursor(8, 5);
    canvas.printf("%.14s %d/%d", active_book_name.c_str(), reader_lines.empty() ? 0 : reader_scroll + 1, static_cast<int>(reader_lines.size()));
    canvas.setTextSize(2);
    canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    for (int row = 0; row < READER_LINES_PER_PAGE; ++row) {
        int idx = reader_scroll + row;
        if (idx >= static_cast<int>(reader_lines.size())) break;
        canvas.setCursor(8, 22 + row * 24);
        drawTextLineSmart(8, 22 + row * 24, reader_lines[idx]);
    }
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_DARKGREY, TFT_BLACK);
    canvas.setCursor(8, 122);
    canvas.print("UP/DN LINE  L/R PAGE  1 SPEED");
    canvas.pushSprite(0, 0);
}

const char* speedModeName()
{
    if (speed_mode == SpeedMode::TwoWords) return "2W";
    if (speed_mode == SpeedMode::Line) return "LINE";
    return "1W";
}

std::string currentSpeedText()
{
    if (speed_mode == SpeedMode::Line) {
        if (reader_lines.empty()) return "";
        return reader_lines[std::max(0, std::min(speed_index, static_cast<int>(reader_lines.size()) - 1))];
    }
    if (reader_words.empty()) return "";
    int idx = std::max(0, std::min(speed_index, static_cast<int>(reader_words.size()) - 1));
    std::string out = reader_words[idx];
    if (speed_mode == SpeedMode::TwoWords && idx + 1 < static_cast<int>(reader_words.size())) {
        out += " ";
        out += reader_words[idx + 1];
    }
    return out;
}

void drawCenteredText(const std::string& text, int y, int text_size, uint16_t color)
{
    int cols = utf8Columns(text);
    int width = cols * 6 * text_size;
    int x = std::max(0, (SCREEN_W - width) / 2);
    canvas.setTextSize(text_size);
    canvas.setTextColor(color, TFT_BLACK);
    canvas.setCursor(x, y);
    canvas.print(text.c_str());
}

void drawReaderSpeed()
{
    canvas.fillScreen(TFT_BLACK);
    canvas.setTextSize(2);
    canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    canvas.setCursor(8, 6);
    canvas.print("SPEED");
    canvas.setCursor(92, 6);
    canvas.printf("%s", speedModeName());
    canvas.setCursor(8, 30);
    canvas.printf("%d WPM", speed_wpm);
    canvas.setCursor(136, 30);
    canvas.print(speed_paused ? "PAUSE" : "RUN");

    std::string text = currentSpeedText();
    bool cyr = containsCyrillic(text);
    int cols = utf8Columns(text);
    int size = cols <= 10 ? 3 : 2;
    if (text.size() > 48) text = text.substr(0, 48);
    if (cyr) {
        int width = cols * 6 * size;
        drawMixedTextLine(std::max(0, (SCREEN_W - width) / 2), size == 3 ? 62 : 66, text, size);
    } else {
        drawCenteredText(text, size == 3 ? 62 : 66, size, speed_paused ? TFT_DARKGREY : TFT_WHITE);
    }

    canvas.setTextSize(1);
    canvas.setTextColor(TFT_DARKGREY, TFT_BLACK);
    canvas.setCursor(8, 106);
    int total = speed_mode == SpeedMode::Line ? static_cast<int>(reader_lines.size()) : static_cast<int>(reader_words.size());
    canvas.printf("POS %d/%d", std::min(speed_index + 1, std::max(1, total)), total);
    canvas.setCursor(8, 122);
    canvas.print("OK RUN/PAUSE  UP/DN WPM  L/R MODE");
    canvas.pushSprite(0, 0);
}


uint32_t elapsedClockSeconds()
{
    return clock_seconds + (M5.millis() - clock_base_ms) / 1000;
}

void formatHMS(uint32_t total, char* out, size_t out_len)
{
    uint32_t h = (total / 3600) % 24;
    uint32_t m = (total / 60) % 60;
    uint32_t sec = total % 60;
    snprintf(out, out_len, "%02lu:%02lu:%02lu", static_cast<unsigned long>(h), static_cast<unsigned long>(m), static_cast<unsigned long>(sec));
}

uint32_t stopwatchDisplayMs()
{
    return stopwatch_elapsed_ms + (stopwatch_running ? M5.millis() - stopwatch_started_ms : 0);
}

uint32_t timerDisplayMs()
{
    if (!timer_running) return timer_remaining_ms;
    uint32_t elapsed = M5.millis() - timer_started_ms;
    return elapsed >= timer_remaining_ms ? 0 : timer_remaining_ms - elapsed;
}

void drawBigTime(const char* text, int y)
{
    canvas.setTextSize(3);
    canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    int width = std::strlen(text) * 18;
    canvas.setCursor(std::max(0, (SCREEN_W - width) / 2), y);
    canvas.print(text);
}


const char* timeFieldName()
{
    if (time_set_field == TimeSetField::Hours) return "HOUR";
    if (time_set_field == TimeSetField::Seconds) return "SEC";
    return "MIN";
}

int timeFieldStepSeconds()
{
    if (time_set_field == TimeSetField::Hours) return 3600;
    if (time_set_field == TimeSetField::Seconds) return 1;
    return 60;
}

void startAlert(uint32_t ms)
{
    alert_until_ms = M5.millis() + ms;
    last_alert_beep_ms = 0;
    M5.Speaker.begin();
}

void updateAlert()
{
    uint32_t now = M5.millis();
    if (alert_until_ms == 0 || now > alert_until_ms) return;
    if (last_alert_beep_ms == 0 || now - last_alert_beep_ms > 650) {
        M5.Speaker.tone(880, 160);
        last_alert_beep_ms = now;
    }
}

void drawTimeApp()
{
    canvas.fillScreen(TFT_BLACK);
    canvas.setTextSize(2);
    canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    canvas.setCursor(8, 6);
    canvas.print("TIME");
    canvas.setCursor(88, 6);
    const char* mode = time_mode == TimeMode::Clock ? "CLOCK" : (time_mode == TimeMode::Stopwatch ? "STOP" : (time_mode == TimeMode::Timer ? "TIMER" : "ALARM"));
    canvas.print(mode);

    char buf[16];
    if (time_mode == TimeMode::Clock) {
        formatHMS(elapsedClockSeconds(), buf, sizeof(buf));
        drawBigTime(buf, 48);
        canvas.setTextSize(1);
        canvas.setTextColor(TFT_DARKGREY, TFT_BLACK);
        canvas.setCursor(8, 96);
        canvas.printf("SET:%s  1 FIELD", timeFieldName());
    } else if (time_mode == TimeMode::Stopwatch) {
        uint32_t ms = stopwatchDisplayMs();
        uint32_t sec = ms / 1000;
        snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu.%lu", static_cast<unsigned long>(sec / 3600), static_cast<unsigned long>((sec / 60) % 60), static_cast<unsigned long>(sec % 60), static_cast<unsigned long>((ms / 100) % 10));
        canvas.setTextSize(2);
        canvas.setTextColor(TFT_WHITE, TFT_BLACK);
        canvas.setCursor(8, 52);
        canvas.print(buf);
        canvas.setTextSize(1);
        canvas.setTextColor(TFT_DARKGREY, TFT_BLACK);
        canvas.setCursor(8, 96);
        canvas.print(stopwatch_running ? "RUN" : "PAUSE");
    } else if (time_mode == TimeMode::Timer) {
        uint32_t sec = (timerDisplayMs() + 999) / 1000;
        snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", static_cast<unsigned long>(sec / 3600), static_cast<unsigned long>((sec / 60) % 60), static_cast<unsigned long>(sec % 60));
        drawBigTime(buf, 48);
        canvas.setTextSize(1);
        canvas.setTextColor(timer_done ? TFT_WHITE : TFT_DARKGREY, TFT_BLACK);
        canvas.setCursor(8, 96);
        canvas.printf("%s SET:%s", timer_done ? "DONE" : (timer_running ? "RUN" : "SET"), timeFieldName());
    } else {
        formatHMS(alarm_seconds, buf, sizeof(buf));
        drawBigTime(buf, 48);
        canvas.setTextSize(1);
        canvas.setTextColor((alarm_enabled || alarm_ringing) ? TFT_WHITE : TFT_DARKGREY, TFT_BLACK);
        canvas.setCursor(8, 96);
        canvas.printf("%s SET:%s", alarm_ringing ? "RING" : (alarm_enabled ? "ON" : "OFF"), timeFieldName());
    }
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_DARKGREY, TFT_BLACK);
    canvas.setCursor(8, 122);
    canvas.print("OK ON/START  1 FIELD/RST  L/R MODE");
    canvas.pushSprite(0, 0);
}

void updateTimeApp()
{
    if (screen != Screen::TimeApp) return;
    static uint32_t last_time_redraw = 0;
    uint32_t now = M5.millis();
    if (timer_running && now - timer_started_ms >= timer_remaining_ms) {
        timer_running = false;
        timer_remaining_ms = 0;
        timer_done = true;
        startAlert(6000);
        dirty = true;
    }
    updateAlert();
    if (alarm_enabled && !alarm_ringing) {
        uint32_t now_clock = elapsedClockSeconds();
        uint32_t day = now_clock / 86400;
        int today_sec = now_clock % 86400;
        if (today_sec >= alarm_seconds && day != last_alarm_day) {
            alarm_ringing = true;
            last_alarm_day = day;
            startAlert(12000);
            dirty = true;
        }
    }
    if (now - last_time_redraw >= (time_mode == TimeMode::Stopwatch && stopwatch_running ? 100 : 1000)) {
        last_time_redraw = now;
        if (!display_off) dirty = true;
    }
}


void drawFilesList()
{
    canvas.fillScreen(TFT_BLACK);
    canvas.setTextSize(2);
    canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    canvas.setCursor(8, 8);
    canvas.printf("FILES %d/%d", file_entries.empty() ? 0 : files_cursor + 1, static_cast<int>(file_entries.size()));
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_DARKGREY, TFT_BLACK);
    canvas.setCursor(8, 30);
    canvas.printf("%.28s", files_path == MOUNT_POINT ? "/" : files_path.substr(std::strlen(MOUNT_POINT)).c_str());
    if (file_entries.empty()) {
        canvas.setTextSize(2);
        canvas.setTextColor(TFT_WHITE, TFT_BLACK);
        canvas.setCursor(8, 54);
        canvas.print(sd_ready ? "EMPTY" : "NO SD");
    } else {
        int rows = 4;
        int start = std::max(0, files_cursor - 1);
        start = std::min(start, std::max(0, static_cast<int>(file_entries.size()) - rows));
        int end = std::min(static_cast<int>(file_entries.size()), start + rows);
        canvas.setTextSize(2);
        for (int i = start; i < end; ++i) {
            const auto& e = file_entries[i];
            canvas.setCursor(8, 42 + (i - start) * 20);
            canvas.setTextColor(i == files_cursor ? TFT_BLACK : TFT_WHITE, i == files_cursor ? TFT_WHITE : TFT_BLACK);
            canvas.printf("%c%c%.12s", i == files_cursor ? '>' : ' ', e.is_dir ? '/' : ' ', e.name.c_str());
        }
    }
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_DARKGREY, TFT_BLACK);
    canvas.setCursor(8, 122);
    canvas.print("OK OPEN  GO BACK  KNOWN FILES");
    canvas.pushSprite(0, 0);
}

bool openFileEntry(const FileEntry& e, std::string* err = nullptr)
{
    if (e.is_dir) {
        if (e.name == "..") scanFiles(parentPath(files_path));
        else scanFiles(e.path);
        screen = Screen::FilesList;
        return true;
    }
    std::string ext = lowerExt(e.name);
    if (ext == ".txt") {
        active_book_name = e.name;
        active_note_name = e.name;
        if (!loadTextFile(e.path, e.name, err)) return false;
        screen = e.path.rfind(std::string(NOTES_DIR) + "/", 0) == 0 ? Screen::NotesView : Screen::ReaderView;
        return true;
    }
    if (ext == ".mp3") {
        override_music_path = e.path;
        if (!startPlayback(err)) return false;
        return true;
    }
    if (ext == ".wav" || ext == ".pcm") {
        rec_play_file = fopen(e.path.c_str(), "rb");
        if (!rec_play_file) {
            if (err) { *err = "open: "; *err += std::strerror(errno); }
            return false;
        }
        fseek(rec_play_file, 44, SEEK_SET);
        rec_buffer.assign(REC_BUFFER_SAMPLES * 4, 0);
        rec_play_chunks = 0;
        active_recording_name = e.name;
        M5.Mic.end();
        M5.Speaker.begin();
        applyVolume();
        screen = Screen::RecorderPlaying;
        return true;
    }
    if (err) *err = "unsupported";
    return false;
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
    else if (screen == Screen::ReaderList) drawReaderList();
    else if (screen == Screen::ReaderView) drawReaderView();
    else if (screen == Screen::ReaderSpeed) drawReaderSpeed();
    else if (screen == Screen::NotesList) drawNotesList();
    else if (screen == Screen::NotesView) drawNotesView();
    else if (screen == Screen::NotesEdit) drawNotesEdit();
    else if (screen == Screen::RecorderList) drawRecorderList();
    else if (screen == Screen::RecorderRecording) drawRecorderRecording();
    else if (screen == Screen::RecorderPlaying) drawRecorderPlaying();
    else if (screen == Screen::TimeApp) drawTimeApp();
    else if (screen == Screen::FilesList) drawFilesList();
    else drawMessage();
    dirty = false;
}

void updateSpeedReader()
{
    if (screen != Screen::ReaderSpeed || speed_paused) return;
    uint32_t now = M5.millis();
    if (now < speed_next_ms) return;
    int total = speed_mode == SpeedMode::Line ? static_cast<int>(reader_lines.size()) : static_cast<int>(reader_words.size());
    if (total <= 0) {
        speed_paused = true;
        dirty = true;
        return;
    }
    int step = speed_mode == SpeedMode::Line ? 1 : speedStepWords();
    speed_index = std::min(total - 1, speed_index + step);
    if (speed_index >= total - 1) speed_paused = true;
    saveReaderBookmark();
    speed_next_ms = now + speedIntervalMs();
    if (!display_off) dirty = true;
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
    if (ev.key == Key::None && screen != Screen::NotesEdit) return;
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
            else if (launcher_index == 1) { scanBooks(); screen = Screen::ReaderList; }
            else if (launcher_index == 2) { scanNotes(); screen = Screen::NotesList; }
            else if (launcher_index == 3) { scanRecordings(); screen = Screen::RecorderList; }
            else if (launcher_index == 4) { time_mode = TimeMode::Clock; clock_base_ms = M5.millis(); screen = Screen::TimeApp; blockInput(250); }
            else if (launcher_index == 5) { scanFiles(MOUNT_POINT); screen = Screen::FilesList; blockInput(250); }
            else { message_title = "Coming soon"; message_body = "Music/Reader/Record"; message_returns_music = false; screen = Screen::Message; }
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
                override_music_path.clear();
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
            alarm_ringing = false;
            alert_until_ms = 0;
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

    if (screen == Screen::ReaderList) {
        if (ev.key == Key::Up && !books.empty()) selected_book = std::max(0, selected_book - 1);
        else if (ev.key == Key::Down && !books.empty()) selected_book = std::min(static_cast<int>(books.size()) - 1, selected_book + 1);
        else if (ev.key == Key::Left && !books.empty()) selected_book = std::max(0, selected_book - 4);
        else if (ev.key == Key::Right && !books.empty()) selected_book = std::min(static_cast<int>(books.size()) - 1, selected_book + 4);
        else if (ev.key == Key::Ok) {
            if (!books.empty()) {
                std::string err;
                if (loadSelectedBook(&err)) {
                    screen = Screen::ReaderView;
                    blockInput(300);
                } else {
                    message_title = "Read failed";
                    message_body = err.empty() ? "open" : err;
                    message_returns_music = false;
                    message_returns_notes = false;
                    screen = Screen::Message;
                    blockInput(350);
                }
            }
        } else if (ev.key == Key::Home || ev.key == Key::Back) {
            screen = Screen::Launcher;
            blockInput(250);
        }
        dirty = true;
        return;
    }

    if (screen == Screen::ReaderView) {
        const int max_scroll = std::max(0, static_cast<int>(reader_lines.size()) - READER_LINES_PER_PAGE);
        if (ev.key == Key::Up) { reader_scroll = std::max(0, reader_scroll - 1); saveReaderBookmark(); }
        else if (ev.key == Key::Down) { reader_scroll = std::min(max_scroll, reader_scroll + 1); saveReaderBookmark(); }
        else if (ev.key == Key::Left) { reader_scroll = std::max(0, reader_scroll - READER_LINES_PER_PAGE); saveReaderBookmark(); }
        else if (ev.key == Key::Right) { reader_scroll = std::min(max_scroll, reader_scroll + READER_LINES_PER_PAGE); saveReaderBookmark(); }
        else if (ev.key == Key::One) {
            speed_mode = SpeedMode::OneWord;
            speed_index = wordIndexForLine(reader_scroll);
            speed_paused = true;
            speed_next_ms = M5.millis() + speedIntervalMs();
            screen = Screen::ReaderSpeed;
            blockInput(300);
        } else if (ev.key == Key::Home || ev.key == Key::Back) {
            saveReaderBookmark();
            screen = Screen::ReaderList;
            blockInput(250);
        }
        dirty = true;
        return;
    }

    if (screen == Screen::ReaderSpeed) {
        if (ev.key == Key::Ok) {
            speed_paused = !speed_paused;
            speed_next_ms = M5.millis() + speedIntervalMs();
        } else if (ev.key == Key::Up) speed_wpm = std::min(SPEED_WPM_MAX, speed_wpm + SPEED_WPM_STEP);
        else if (ev.key == Key::Down) speed_wpm = std::max(SPEED_WPM_MIN, speed_wpm - SPEED_WPM_STEP);
        else if (ev.key == Key::Left) {
            int mode = static_cast<int>(speed_mode);
            setSpeedMode(static_cast<SpeedMode>((mode + 2) % 3));
            saveReaderBookmark();
        } else if (ev.key == Key::Right) {
            int mode = static_cast<int>(speed_mode);
            setSpeedMode(static_cast<SpeedMode>((mode + 1) % 3));
            saveReaderBookmark();
        } else if (ev.key == Key::Home || ev.key == Key::Back) {
            reader_scroll = speed_mode == SpeedMode::Line ? speed_index : lineIndexForWord(speed_index);
            reader_scroll = std::min(reader_scroll, std::max(0, static_cast<int>(reader_lines.size()) - READER_LINES_PER_PAGE));
            saveReaderBookmark();
            screen = Screen::ReaderView;
            blockInput(250);
        }
        dirty = true;
        return;
    }

    if (screen == Screen::NotesList) {
        const int total = static_cast<int>(notes.size()) + 1;
        if (ev.key == Key::Up) notes_cursor = std::max(0, notes_cursor - 1);
        else if (ev.key == Key::Down) notes_cursor = std::min(total - 1, notes_cursor + 1);
        else if (ev.key == Key::Left) notes_cursor = std::max(0, notes_cursor - 4);
        else if (ev.key == Key::Right) notes_cursor = std::min(total - 1, notes_cursor + 4);
        else if (ev.key == Key::Ok) {
            if (notes_cursor == 0) {
                note_input.clear();
                note_ru_mode = false;
                screen = Screen::NotesEdit;
                blockInput(300);
            } else {
                std::string err;
                if (loadSelectedNote(&err)) {
                    screen = Screen::NotesView;
                    blockInput(300);
                } else {
                    message_title = "Note failed";
                    message_body = err.empty() ? "open" : err;
                    message_returns_music = false;
                    message_returns_notes = true;
                    screen = Screen::Message;
                }
            }
        } else if (ev.key == Key::Home || ev.key == Key::Back) {
            screen = Screen::Launcher;
            blockInput(250);
        }
        dirty = true;
        return;
    }

    if (screen == Screen::NotesView) {
        const int max_scroll = std::max(0, static_cast<int>(reader_lines.size()) - READER_LINES_PER_PAGE);
        if (ev.key == Key::Up) { reader_scroll = std::max(0, reader_scroll - 1); saveReaderBookmark(); }
        else if (ev.key == Key::Down) { reader_scroll = std::min(max_scroll, reader_scroll + 1); saveReaderBookmark(); }
        else if (ev.key == Key::Left) { reader_scroll = std::max(0, reader_scroll - READER_LINES_PER_PAGE); saveReaderBookmark(); }
        else if (ev.key == Key::Right) { reader_scroll = std::min(max_scroll, reader_scroll + READER_LINES_PER_PAGE); saveReaderBookmark(); }
        else if (ev.key == Key::Home || ev.key == Key::Back) {
            screen = Screen::NotesList;
            blockInput(250);
        }
        dirty = true;
        return;
    }

    if (screen == Screen::NotesEdit) {
        if (ev.key == Key::One) {
            note_ru_mode = !note_ru_mode;
            blockInput(250);
        } else if (ev.key == Key::Ok) {
            if (note_input.empty()) {
                message_title = "Note empty";
                message_body = "not saved";
                screen = Screen::NotesList;
            } else {
                std::string name;
                std::string err;
                if (saveNewNote(&name, &err)) {
                    message_title = "Note saved";
                    message_body = name;
                    message_returns_music = false;
                    message_returns_notes = true;
                    screen = Screen::Message;
                } else {
                    message_title = "Save failed";
                    message_body = err.empty() ? "write" : err;
                    message_returns_music = false;
                    message_returns_notes = true;
                    screen = Screen::Message;
                }
            }
            blockInput(400);
        } else if (ev.key == Key::Backspace) {
            if (!note_input.empty()) note_input.pop_back();
        } else if (ev.key == Key::Home || ev.key == Key::Back) {
            note_input.clear();
            screen = Screen::NotesList;
            blockInput(250);
        } else if (ev.key == Key::None && ev.name && ev.name[0] && !ev.name[1]) {
            char c = ev.name[0];
            if (static_cast<unsigned char>(c) >= 32 && static_cast<unsigned char>(c) <= 126 && note_input.size() < 512) {
                note_input.push_back(c);
            }
        }
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
                    message_returns_notes = false;
                    screen = Screen::Message;
                }
            } else {
                if (!startRecordingPlayback(&err)) {
                    message_title = "Play failed";
                    message_body = err.empty() ? "open" : err;
                    message_returns_music = false;
                    message_returns_notes = false;
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

    if (screen == Screen::TimeApp) {
        if (ev.key == Key::Left) {
            time_mode = static_cast<TimeMode>((static_cast<int>(time_mode) + 3) % 4);
        } else if (ev.key == Key::Right) {
            time_mode = static_cast<TimeMode>((static_cast<int>(time_mode) + 1) % 4);
        } else if (ev.key == Key::Ok) {
            if (time_mode == TimeMode::Stopwatch) {
                if (stopwatch_running) {
                    stopwatch_elapsed_ms = stopwatchDisplayMs();
                    stopwatch_running = false;
                } else {
                    stopwatch_started_ms = M5.millis();
                    stopwatch_running = true;
                }
            } else if (time_mode == TimeMode::Timer) {
                if (timer_running) {
                    timer_remaining_ms = timerDisplayMs();
                    timer_running = false;
                } else {
                    if (timer_remaining_ms == 0) timer_remaining_ms = timer_seconds * 1000UL;
                    timer_started_ms = M5.millis();
                    timer_running = true;
                    timer_done = false;
                }
            } else if (time_mode == TimeMode::Alarm) {
                if (alarm_ringing) {
                    alarm_ringing = false;
                    alert_until_ms = 0;
                } else {
                    alarm_enabled = !alarm_enabled;
                    if (alarm_enabled) last_alarm_day = 999999;
                }
            }
        } else if (ev.key == Key::One) {
            if (time_mode == TimeMode::Stopwatch) {
                stopwatch_running = false;
                stopwatch_elapsed_ms = 0;
            } else if (time_mode == TimeMode::Timer) {
                if (timer_running || timer_done) {
                    timer_running = false;
                    timer_done = false;
                    timer_remaining_ms = timer_seconds * 1000UL;
                    alert_until_ms = 0;
                } else {
                    time_set_field = static_cast<TimeSetField>((static_cast<int>(time_set_field) + 1) % 3);
                }
            } else if (time_mode == TimeMode::Alarm) {
                time_set_field = static_cast<TimeSetField>((static_cast<int>(time_set_field) + 1) % 3);
            } else {
                time_set_field = static_cast<TimeSetField>((static_cast<int>(time_set_field) + 1) % 3);
            }
        } else if (ev.key == Key::Up) {
            int step = timeFieldStepSeconds();
            if (time_mode == TimeMode::Clock) { clock_seconds = (elapsedClockSeconds() + step) % 86400; clock_base_ms = M5.millis(); }
            else if (time_mode == TimeMode::Timer && !timer_running) { timer_seconds = std::min(23 * 3600 + 59 * 60 + 59, timer_seconds + step); timer_remaining_ms = timer_seconds * 1000UL; timer_done = false; }
            else if (time_mode == TimeMode::Alarm) { alarm_seconds = (alarm_seconds + step) % 86400; alarm_ringing = false; }
        } else if (ev.key == Key::Down) {
            int step = timeFieldStepSeconds();
            if (time_mode == TimeMode::Clock) { clock_seconds = (elapsedClockSeconds() + 86400 - step) % 86400; clock_base_ms = M5.millis(); }
            else if (time_mode == TimeMode::Timer && !timer_running) { timer_seconds = std::max(1, timer_seconds - step); timer_remaining_ms = timer_seconds * 1000UL; timer_done = false; }
            else if (time_mode == TimeMode::Alarm) { alarm_seconds = (alarm_seconds + 86400 - step) % 86400; alarm_ringing = false; }
        } else if (ev.key == Key::Home || ev.key == Key::Back) {
            screen = Screen::Launcher;
            blockInput(250);
        }
        dirty = true;
        return;
    }

    if (screen == Screen::FilesList) {
        if (ev.key == Key::Up && !file_entries.empty()) files_cursor = std::max(0, files_cursor - 1);
        else if (ev.key == Key::Down && !file_entries.empty()) files_cursor = std::min(static_cast<int>(file_entries.size()) - 1, files_cursor + 1);
        else if (ev.key == Key::Left && !file_entries.empty()) files_cursor = std::max(0, files_cursor - 4);
        else if (ev.key == Key::Right && !file_entries.empty()) files_cursor = std::min(static_cast<int>(file_entries.size()) - 1, files_cursor + 4);
        else if (ev.key == Key::Ok && !file_entries.empty()) {
            std::string err;
            if (!openFileEntry(file_entries[files_cursor], &err)) {
                message_title = "Open failed";
                message_body = err.empty() ? "unsupported" : err;
                screen = Screen::Message;
            }
            blockInput(350);
        } else if (ev.key == Key::Home || ev.key == Key::Back) {
            if (files_path == MOUNT_POINT) screen = Screen::Launcher;
            else scanFiles(parentPath(files_path));
            blockInput(250);
        }
        dirty = true;
        return;
    }

    if (screen == Screen::Message) {
        if (M5.millis() < message_hold_until_ms) return;
        if (message_returns_music && (ev.key == Key::Home || ev.key == Key::Back || ev.key == Key::Ok)) {
            message_returns_music = false;
            screen = Screen::MusicList;
        } else if (message_returns_notes && (ev.key == Key::Home || ev.key == Key::Back || ev.key == Key::Ok)) {
            message_returns_notes = false;
            scanNotes();
            screen = Screen::NotesList;
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
        updateSpeedReader();
        updateTimeApp();
        updatePower();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
