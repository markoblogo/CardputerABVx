#include <M5Unified.hpp>
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <string>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

#include <driver/sdspi_host.h>
#include <driver/spi_master.h>
#include <esp_app_desc.h>
#include <esp_event.h>
#include <esp_http_server.h>
#include <esp_http_client.h>
#include <esp_heap_caps.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <esp_spiffs.h>
#include "persistence.h"
#include <esp_vfs_fat.h>
#include <esp_wifi.h>
#include <ff.h>
#include <freertos/semphr.h>
#include <nvs_flash.h>
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
constexpr const char* MUSIC_INDEX_FILE = "/sdcard/music/INDEX.TXT";
constexpr const char* BOOKS_DIR = "/sdcard/books";
constexpr const char* NOTES_DIR = "/sdcard/notes";
constexpr const char* INBOX_FILE = "/voice/INBOX.LOG";
constexpr const char* INBOX_TMP_FILE = "/voice/INBOX.NEW";
constexpr const char* RECORDINGS_DIR = "/sdcard/rec";
constexpr const char* RECORDINGS_FALLBACK_DIR = "/sdcard/RECS";
constexpr const char* RECORDINGS_LEGACY_DIR = "/sdcard/REC";
constexpr const char* RECORDINGS_COMPAT_DIR = "/sdcard/recordings";
constexpr const char* HABITS_DIR = "/sdcard/habits";
constexpr const char* HABITS_FILE = "/sdcard/habits/HABITS.TXT";
constexpr const char* HABIT_LOG_FILE = "/sdcard/habits/LOG.TXT";
constexpr const char* HABIT_LOG_TMP_FILE = "/sdcard/habits/LOG.NEW";
constexpr const char* HABIT_STATE_FILE = "/sdcard/habits/STATE.TXT";
constexpr const char* CONFIG_DIR = "/sdcard/CARDPTR";
constexpr const char* CONFIG_FILE = "/sdcard/CARDPTR/CONFIG.TXT";
constexpr const char* VOICE_STORE_DIR = "/voice";
constexpr const char* VOICE_STORE_LABEL = "voice";
constexpr const char* READER_STATE_FILE = "/voice/READER.STA";
constexpr const char* READER_STATE_TMP_FILE = "/voice/READER.NEW";
constexpr const char* READER_STATE_LEGACY_FILE = "/sdcard/CARDPTR/READER.TXT";
constexpr int SCREEN_W = 240;
constexpr int SCREEN_H = 135;
// Keep the two queued PCM buffers small enough for a large music library.
// 59 long filenames plus two 900 ms buffers pushed the no-PSRAM Cardputer
// over its practical heap limit while preparing the second audio chunk.
constexpr int INPUT_BUF_SIZE = 16 * 1024;
constexpr int CHUNK_MS = 400;
constexpr int AUDIO_OUTPUT_RATE = 16000;
constexpr bool AUDIO_SAFE_MONO = true;
constexpr int AUDIO_SAFE_SHIFT = 2;
constexpr size_t MAX_BOOK_BYTES = 128 * 1024;
constexpr size_t READER_STREAM_READ_BYTES = 2048;
constexpr size_t MAX_UPLOAD_BYTES = 32 * 1024 * 1024;
constexpr size_t MAX_DIRECT_UPLOAD_BYTES = 64 * 1024;
constexpr size_t MAX_UPLOAD_CHUNK_BYTES = 2 * 1024;
constexpr uint32_t UPLOAD_OP_TIMEOUT_MS = 15000;
constexpr uint32_t UPLOAD_SESSION_TIMEOUT_MS = 45000;
constexpr size_t MAX_LIST_ENTRIES = 256;
constexpr size_t MAX_STATE_RECORDS = 256;
constexpr size_t MAX_HABITS = 64;
constexpr int READER_LINES_PER_PAGE = 4;
constexpr int READER_STREAM_LOOKAHEAD_LINES = READER_LINES_PER_PAGE + 1;
constexpr int SPEED_WPM_MIN = 350;
constexpr int SPEED_WPM_MAX = 1000;
constexpr int SPEED_WPM_STEP = 50;
constexpr uint32_t CAST_REQUEST_TIMEOUT_MS = 1500;
constexpr int CAST_MAX_HOST = 63;
constexpr size_t CAST_MAX_TRACE_LINE = 64;
constexpr uint32_t DEBOUNCE_MS = 180;
constexpr float PI = 3.14159265358979323846f;

bool isCastHostChar(char c);

Adafruit_TCA8418 keyboard;
sdmmc_card_t* sd_card = nullptr;
bool spi_ready = false;
bool sd_ready = false;
httpd_handle_t connection_httpd = nullptr;
esp_netif_t* connection_ap_netif = nullptr;
bool connection_stack_ready = false;
bool connection_wifi_on = false;
bool connection_http_on = false;
volatile bool connection_dirty = false;
int connection_req_count = 0;
char connection_last_endpoint[32] = "-";
char connection_last_error[64] = "none";
char connection_ap_password[16] = "cardputer";
volatile bool connection_upload_active = false;
volatile int connection_upload_done = 0;
volatile int connection_upload_total = 0;
portMUX_TYPE connection_time_mux = portMUX_INITIALIZER_UNLOCKED;
volatile bool connection_time_sync_pending = false;
uint64_t connection_time_pending_epoch = 0;
int connection_time_pending_offset_min = 0;
uint64_t connection_time_last_epoch = 0;
int connection_time_last_offset_min = 0;
bool connection_time_sync_applied = false;
char cast_host[CAST_MAX_HOST + 1] = "192.168.4.1";
uint16_t cast_port = 8080;
bool cast_trace_enabled = true;
char cast_last_endpoint[48] = "-";
char cast_last_path[96] = "-";
char cast_last_error[64] = "none";
uint16_t cast_last_code = 0;
uint16_t cast_last_latency_ms = 0;
uint16_t cast_last_status_ms = 0;
bool cast_last_ok = false;
uint32_t cast_retries_total = 0;
bool settings_cast_editing = false;
bool settings_cast_edit_host = false;
std::string settings_cast_edit_buffer;

enum class ConnectionUploadOp { None, Begin, Chunk, Finish, Abort };
SemaphoreHandle_t connection_upload_mutex = nullptr;
SemaphoreHandle_t connection_upload_complete = nullptr;
ConnectionUploadOp connection_pending_op = ConnectionUploadOp::None;
bool connection_pending_done = false;
bool connection_pending_ok = false;
std::string connection_pending_api_path;
std::string connection_pending_full_path;
std::string connection_pending_original_name;
std::string connection_pending_error;
std::vector<uint8_t> connection_pending_chunk;
size_t connection_pending_total = 0;
size_t connection_pending_offset = 0;
FILE* connection_upload_file = nullptr;
bool connection_upload_session = false;
std::string connection_upload_path;
std::string connection_upload_original_name;
std::string connection_upload_final_path;
std::string connection_upload_part_path;
uint32_t connection_upload_last_activity_ms = 0;

LGFX_Sprite canvas(&M5.Display);

enum class Screen { Launcher, Dashboard, MusicList, MusicInfo, MusicPlaying, ReaderList, ReaderView, ReaderSpeed, NotesList, NotesView, NotesEdit, NotesDeleteConfirm, RecorderList, RecorderRecording, RecorderPlaying, RecorderDeleteConfirm, TimeApp, FilesList, FilesInfo, FilesDeleteConfirm, Randomizer, HabitsList, HabitsStats, HabitsManage, HabitsEdit, HabitsDisableConfirm, Settings, Connections, InboxList, InboxDetail, Message };
enum class Key { None, Up, Down, Left, Right, Ok, Back, Home, One, Two, Backspace };
enum class VolumeMode { Mute = 0, Mid = 1, Loud = 2 };
enum class SpeedMode { OneWord = 0, TwoWords = 1, Line = 2 };
enum class TimeMode { Clock = 0, Stopwatch = 1, Timer = 2, Alarm = 3 };
enum class TimeSetField { Hours = 0, Minutes = 1, Seconds = 2 };
enum class ThemeMode { White = 0, Green = 1, Yellow = 2, Invert = 3 };
enum class SoundMode { Off = 0, Low = 1, Mid = 2, Loud = 3, Max = 4 };
enum class TimeoutMode { Short = 0, Normal = 1, Long = 2 };
enum class MessageReturn { Launcher, Music, Notes, Files, Recorder, Habits };
enum class ResumeTarget { Music = 0, Reader = 1, Notes = 2, Recorder = 3, Time = 4, Files = 5, Randomizer = 6, Habits = 7, Settings = 8, Connections = 9 };

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

struct Habit {
    std::string id;
    std::string title;
    bool active = true;
    bool done = false;
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
uint32_t ui_anim_until_ms = 0;
uint32_t ui_anim_last_frame_ms = 0;
uint32_t marquee_last_frame_ms = 0;

Screen screen = Screen::Launcher;
int launcher_index = 0;
std::string message_title;
std::string message_body;
MessageReturn message_return = MessageReturn::Launcher;
uint32_t message_hold_until_ms = 0;
ResumeTarget last_resume_target = ResumeTarget::Music;

std::vector<std::string> tracks;
std::map<std::string, std::string> music_titles;
std::map<std::string, std::string> music_physical_names;
std::vector<std::string> books;
std::vector<std::string> notes;
std::vector<std::string> recordings;
std::vector<std::string> inbox_entries;
std::vector<std::string> inbox_pending_events;
std::string inbox_status = "INTERNAL";
bool inbox_persist_requested = false;
std::vector<FileEntry> file_entries;
FileEntry file_info_entry;
std::vector<Habit> habits;
int disabled_habit_count = 0;
std::string pending_delete_path;
std::string pending_delete_name;
std::string files_path = MOUNT_POINT;
std::string files_status = "EMPTY";
int files_cursor = 0;
int habits_cursor = 0;
int habit_day = 1;
int habit_stats_window = 7;
int habits_manage_cursor = 0;
std::string habit_input;
std::string habit_edit_id;
bool habit_edit_renaming = false;
std::string pending_habit_id;
std::string pending_habit_name;
int settings_cursor = 0;
ThemeMode theme_mode = ThemeMode::White;
SoundMode sound_mode = SoundMode::Mid;
TimeoutMode timeout_mode = TimeoutMode::Normal;
bool power_save = false;
std::string config_status = "RAM";
std::string recordings_dir = VOICE_STORE_DIR;
int selected_track = 0;
std::string override_music_path;
std::string music_scan_status = "NOT SCANNED";
size_t music_raw_entries = 0;
size_t music_mp3_entries = 0;
std::string music_info_title;
std::string music_info_file;
std::string music_info_size = "-";
std::string music_info_status = "READY";
int music_info_index = 0;
int music_info_total = 0;
int selected_book = 0;
std::string last_reader_book;
int notes_cursor = 0;
int inbox_cursor = 0;
int inbox_flushed_last = 0;
int selected_recording = 0;
bool shuffle_on = false;
std::vector<int> shuffle_order;
int shuffle_position = -1;
VolumeMode volume_mode = VolumeMode::Mid;
uint32_t last_input_ms = 0;
bool display_off = false;
bool display_dim = false;
bool dirty = true;
int battery_last_level = -1;
int battery_last_mv = -1;

FIL mp3_fat_file = {};
bool mp3_fat_open = false;
mp3dec_t mp3_dec;
std::vector<uint8_t> mp3_buf;
size_t mp3_len = 0;
size_t mp3_pos = 0;
bool mp3_eof = false;
bool playing = false;
bool music_autostart_pending = false;
uint32_t playback_decode_after_ms = 0;
std::vector<int16_t> pcm_chunk;
std::vector<int16_t> pcm_next_chunk;
bool pcm_next_ready = false;
std::vector<mp3d_sample_t> mp3_frame_pcm;
int pcm_rate = 44100;
int pcm_channels = 2;
int decoded_chunks = 0;
int music_underruns = 0;

constexpr int REC_SAMPLE_RATE = 16000;
// 4 kHz / 8-bit keeps a full 20-second voice note in ~80 KB. The Cardputer
// cannot reliably reserve the ~160 KB required by 8 kHz alongside the UI.
constexpr int REC_RECORD_SAMPLE_RATE = 4000;
constexpr uint16_t REC_RECORD_BITS = 8;
constexpr uint32_t REC_RECORD_SECONDS = 20;
constexpr size_t REC_BUFFER_SAMPLES = 1024;
constexpr uint32_t REC_MIN_SECONDS = 1;
constexpr uint32_t REC_STOP_GRACE_MS = 120;
constexpr size_t REC_SAVE_CHUNK_SAMPLES = 256;
constexpr size_t REC_CAPTURE_CHUNK_BYTES = 8192;
FILE* rec_play_file = nullptr;
std::vector<int16_t> rec_buffer;
std::vector<int16_t> rec_play_all;
std::vector<int16_t> rec_play_next_chunk;
bool rec_play_current_last = false;
bool rec_play_next_last = false;
struct RecCaptureChunk {
    uint8_t* data = nullptr;
    size_t used = 0;
    size_t capacity = 0;
};
std::vector<RecCaptureChunk> rec_capture_chunks;
size_t rec_capture_capacity = 0;
size_t rec_capture_byte_capacity = 0;
size_t rec_capture_bytes_written = 0;
uint32_t rec_samples_written = 0;
uint32_t rec_started_ms = 0;
uint32_t rec_play_chunks = 0;
bool rec_play_next_ready = false;
uint32_t rec_target_seconds = REC_RECORD_SECONDS;
uint32_t rec_requested_seconds = REC_RECORD_SECONDS;
uint32_t rec_target_ms = REC_RECORD_SECONDS * 1000u;
uint32_t rec_record_sample_rate = REC_RECORD_SAMPLE_RATE;
uint16_t rec_record_bits = REC_RECORD_BITS;
uint16_t rec_play_bits = 16;
std::vector<uint8_t> rec_u8_buffer;
std::vector<uint8_t> rec_play_u8_buffer;
bool rec_auto_stopped = false;
bool rec_save_pending = false;
uint32_t rec_save_due_ms = 0;
uint32_t rec_stopped_ms = 0;
uint32_t rec_mic_chunks = 0;
size_t rec_last_take = 0;
int recorder_cursor = 0;
bool rec_write_error = false;
std::string rec_write_error_text;
std::string active_recording_name;
std::string active_book_name;
bool reader_opened_this_session = false;
std::string active_note_name;
std::string last_note_name;
std::string note_input;
bool note_edit_existing = false;
std::string reader_text;
std::vector<std::string> reader_lines;
std::vector<std::string> reader_words;
std::map<std::string, int> reader_bookmarks;
std::map<std::string, size_t> reader_stream_bookmarks;
bool reader_state_save_requested = false;
int reader_scroll = 0;
bool reader_streaming = false;
std::string reader_stream_path;
FILE* reader_stream_file = nullptr;
size_t reader_stream_file_size = 0;
size_t reader_stream_offset = 0;
std::vector<std::string> reader_stream_lines;
std::vector<size_t> reader_stream_line_offsets;
std::vector<size_t> reader_stream_history;
std::string reader_stream_status;
std::string reader_stream_speed_text;
size_t reader_stream_speed_offset = 0;
size_t reader_stream_speed_next_offset = 0;
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
constexpr int TIMER_PRESETS[] = {60, 5 * 60, 10 * 60, 20 * 60};
int timer_preset_index = 1;
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
std::string random_result = "READY";
std::vector<std::string> random_history;

bool initSd();
bool initVoiceStorage();
bool ensureConnectionWriteDir(char* err, size_t err_len);
void clearCaptureChunks();
bool prepareCaptureChunks();
bool appendCaptureChunk(const void* data, size_t byte_count);
uint32_t elapsedClockSeconds();
void formatHMS(uint32_t total, char* out, size_t out_len);

void processConnectionTimeSync()
{
    uint64_t epoch = 0;
    int offset_min = 0;
    bool pending = false;
    portENTER_CRITICAL(&connection_time_mux);
    if (connection_time_sync_pending) {
        epoch = connection_time_pending_epoch;
        offset_min = connection_time_pending_offset_min;
        connection_time_sync_pending = false;
        pending = true;
    }
    portEXIT_CRITICAL(&connection_time_mux);
    if (!pending) return;

    timeval tv = {};
    tv.tv_sec = static_cast<time_t>(epoch);
    settimeofday(&tv, nullptr);
    int64_t local_epoch = static_cast<int64_t>(epoch) + static_cast<int64_t>(offset_min) * 60;
    int64_t day_seconds = local_epoch % 86400;
    if (day_seconds < 0) day_seconds += 86400;
    clock_seconds = static_cast<int>(day_seconds);
    clock_base_ms = M5.millis();
    portENTER_CRITICAL(&connection_time_mux);
    connection_time_last_epoch = epoch;
    connection_time_last_offset_min = offset_min;
    connection_time_sync_applied = true;
    portEXIT_CRITICAL(&connection_time_mux);
    connection_dirty = true;
    dirty = true;
}
bool flushAndClose(FILE* f)
{
    if (!f) return false;
    bool ok = true;
    if (fflush(f) != 0) ok = false;
    if (fclose(f) != 0) ok = false;
    return ok;
}

bool manualSdReprobe()
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    if (sd_card) {
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, sd_card);
    }
    sd_card = nullptr;
    sd_ready = false;
    // A VFS-only remount is insufficient when the Cardputer SD controller is
    // stuck: physically reinserting the card recovers it.  Fully release the
    // SPI host so the next mount sends a clean card-init sequence.
    if (spi_ready) {
        const esp_err_t free_result = spi_bus_free(static_cast<spi_host_device_t>(host.slot));
        if (free_result == ESP_OK) {
            spi_ready = false;
        } else {
            return false;
        }
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    return initSd();
}

bool initVoiceStorage()
{
    static bool initialized = false;
    if (initialized) return true;
    esp_vfs_spiffs_conf_t conf = {};
    conf.base_path = VOICE_STORE_DIR;
    conf.partition_label = VOICE_STORE_LABEL;
    conf.max_files = 8;
    conf.format_if_mount_failed = true;
    if (esp_vfs_spiffs_register(&conf) != ESP_OK) return false;
    initialized = true;
    return true;
}

void clearCaptureChunks()
{
    for (auto& chunk : rec_capture_chunks) {
        if (chunk.data) {
            heap_caps_free(chunk.data);
            chunk.data = nullptr;
        }
        chunk.used = 0;
        chunk.capacity = 0;
    }
    rec_capture_chunks.clear();
    rec_capture_capacity = 0;
    rec_capture_byte_capacity = 0;
    rec_capture_bytes_written = 0;
}

bool prepareCaptureChunks()
{
    if (rec_capture_byte_capacity == 0) return false;
    const size_t chunk_count = (rec_capture_byte_capacity + REC_CAPTURE_CHUNK_BYTES - 1) / REC_CAPTURE_CHUNK_BYTES;
    rec_capture_chunks.reserve(chunk_count);
    size_t remaining = rec_capture_byte_capacity;
    while (remaining > 0) {
        const size_t wanted = std::min(remaining, REC_CAPTURE_CHUNK_BYTES);
        uint8_t* block = static_cast<uint8_t*>(heap_caps_malloc(wanted, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!block) block = static_cast<uint8_t*>(heap_caps_malloc(wanted, MALLOC_CAP_8BIT));
        if (!block) {
            clearCaptureChunks();
            return false;
        }
        rec_capture_chunks.push_back({block, 0, wanted});
        remaining -= wanted;
    }
    return true;
}

bool appendCaptureChunk(const void* data, size_t byte_count)
{
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    size_t offset = 0;
    while (byte_count > 0) {
        RecCaptureChunk* target = nullptr;
        for (auto& chunk : rec_capture_chunks) {
            if (chunk.used < chunk.capacity) {
                target = &chunk;
                break;
            }
        }
        if (!target) return false;
        const size_t room = target->capacity - target->used;
        const size_t n = std::min(room, byte_count);
        std::memcpy(target->data + target->used, bytes + offset, n);
        target->used += n;
        offset += n;
        byte_count -= n;
        rec_capture_bytes_written += n;
    }
    return true;
}

void showMessage(const std::string& title, const std::string& body, MessageReturn ret = MessageReturn::Launcher)
{
    message_title = title;
    message_body = body;
    message_return = ret;
    screen = Screen::Message;
}

uint16_t uiBg()
{
    return theme_mode == ThemeMode::Invert ? 0xFFFF : 0x0000;
}

uint16_t uiFg()
{
    if (theme_mode == ThemeMode::Green) return 0x07E0;
    if (theme_mode == ThemeMode::Yellow) return 0xFFE0;
    return theme_mode == ThemeMode::Invert ? 0x0000 : 0xFFFF;
}

uint16_t uiDim()
{
    if (theme_mode == ThemeMode::Green) return 0x05E0;
    if (theme_mode == ThemeMode::Yellow) return 0xBDE0;
    return theme_mode == ThemeMode::Invert ? 0x8410 : 0x7BEF;
}

uint16_t uiAccent()
{
    if (theme_mode == ThemeMode::White) return 0x07FF;   // white theme: cyan cyber accent
    if (theme_mode == ThemeMode::Green) return 0xFFE0;   // green theme: yellow signal
    if (theme_mode == ThemeMode::Yellow) return 0x07E0;  // yellow theme: green signal
    if (theme_mode == ThemeMode::Invert) return 0xF800;  // inverted theme: red signal
    return 0x07FF;
}

uint16_t uiSignal()
{
    if (theme_mode == ThemeMode::Green) return 0xFFE0;   // green theme: yellow waveform
    if (theme_mode == ThemeMode::Yellow) return 0x07E0;  // yellow theme: green waveform
    if (theme_mode == ThemeMode::Invert) return 0xF800;  // inverted theme: red waveform
    return 0xFFFF;                                       // white theme: white waveform
}

const char* themeName()
{
    if (theme_mode == ThemeMode::Green) return "GREEN";
    if (theme_mode == ThemeMode::Yellow) return "YELLOW";
    if (theme_mode == ThemeMode::Invert) return "INVERT";
    return "WHITE";
}

const char* soundName()
{
    if (sound_mode == SoundMode::Off) return "OFF";
    if (sound_mode == SoundMode::Low) return "LOW";
    if (sound_mode == SoundMode::Loud) return "LOUD";
    if (sound_mode == SoundMode::Max) return "MAX";
    return "MID";
}

const char* timeoutName()
{
    if (timeout_mode == TimeoutMode::Short) return "SHORT";
    if (timeout_mode == TimeoutMode::Long) return "LONG";
    return "NORMAL";
}

int soundVolume()
{
    if (sound_mode == SoundMode::Off) return 0;
    if (sound_mode == SoundMode::Low) return 60;
    if (sound_mode == SoundMode::Loud) return 200;
    if (sound_mode == SoundMode::Max) return 255;
    return 128;
}

void applyPowerSavePreset()
{
    if (!power_save) return;
    theme_mode = ThemeMode::Green;
    sound_mode = SoundMode::Low;
    timeout_mode = TimeoutMode::Short;
}

void setThemeByName(const std::string& value)
{
    if (value == "GREEN") theme_mode = ThemeMode::Green;
    else if (value == "YELLOW") theme_mode = ThemeMode::Yellow;
    else if (value == "INVERT") theme_mode = ThemeMode::Invert;
    else theme_mode = ThemeMode::White;
}

void setSoundByName(const std::string& value)
{
    if (value == "OFF") sound_mode = SoundMode::Off;
    else if (value == "LOW") sound_mode = SoundMode::Low;
    else if (value == "LOUD") sound_mode = SoundMode::Loud;
    else if (value == "MAX") sound_mode = SoundMode::Max;
    else sound_mode = SoundMode::Mid;
}

void setTimeoutByName(const std::string& value)
{
    if (value == "SHORT") timeout_mode = TimeoutMode::Short;
    else if (value == "LONG") timeout_mode = TimeoutMode::Long;
    else timeout_mode = TimeoutMode::Normal;
}

void setCastHost(const std::string& host)
{
    if (host.empty()) return;
    std::string clean;
    for (char c : host) {
        if (std::isspace(static_cast<unsigned char>(c)) || c == '/' || c == '\\') continue;
        if (isCastHostChar(c)) clean.push_back(c);
    }
    if (clean.empty()) return;
    if (clean.size() > CAST_MAX_HOST) clean = clean.substr(0, CAST_MAX_HOST);
    snprintf(cast_host, sizeof(cast_host), "%s", clean.c_str());
}

void setCastPortFromName(const std::string& value)
{
    int parsed = std::atoi(value.c_str());
    if (parsed <= 0 || parsed > 65535) return;
    cast_port = static_cast<uint16_t>(parsed);
}

bool isCastHostChar(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           c == '.' || c == ':' || c == '-' || c == '_' ;
}

void setCastTrace(const char* endpoint, const char* path, bool ok, uint16_t code, uint16_t latency_ms, const char* err = nullptr)
{
    if (endpoint && endpoint[0]) snprintf(cast_last_endpoint, sizeof(cast_last_endpoint), "%s", endpoint);
    else snprintf(cast_last_endpoint, sizeof(cast_last_endpoint), "-");
    if (path && path[0]) snprintf(cast_last_path, sizeof(cast_last_path), "%s", path);
    else snprintf(cast_last_path, sizeof(cast_last_path), "-");
    cast_last_code = code;
    cast_last_latency_ms = latency_ms;
    cast_last_status_ms = static_cast<uint16_t>(M5.millis() & 0xFFFF);
    cast_last_ok = ok;
    if (err && err[0]) snprintf(cast_last_error, sizeof(cast_last_error), "%s", err);
    else snprintf(cast_last_error, sizeof(cast_last_error), ok ? "none" : "error");
}

bool ensureConfigDir()
{
    if (!initSd()) {
        config_status = "RAM";
        return false;
    }
    errno = 0;
    if (mkdir(CONFIG_DIR, 0775) != 0 && errno != EEXIST) {
        config_status = "RAM";
        return false;
    }
    return true;
}

void saveConfig()
{
    if (!ensureConfigDir()) return;
    FILE* f = fopen(CONFIG_FILE, "wb");
    if (!f) {
        config_status = "RAM";
        return;
    }
    applyPowerSavePreset();
    fprintf(f, "THEME=%s\n", themeName());
    fprintf(f, "SOUND=%s\n", soundName());
    fprintf(f, "TIMEOUT=%s\n", timeoutName());
    fprintf(f, "POWER=%d\n", power_save ? 1 : 0);
    fprintf(f, "CAST_HOST=%s\n", cast_host);
    fprintf(f, "CAST_PORT=%u\n", static_cast<unsigned int>(cast_port));
    fprintf(f, "CAST_TRACE=%d\n", cast_trace_enabled ? 1 : 0);
    if (!flushAndClose(f)) {
        config_status = "RAM";
        return;
    }
    config_status = "SAVED";
}

void loadConfig()
{
    config_status = "RAM";
    if (!ensureConfigDir()) return;
    FILE* f = fopen(CONFIG_FILE, "rb");
    if (!f) {
        saveConfig();
        return;
    }
    char line[80];
    while (fgets(line, sizeof(line), f)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.rfind("THEME=", 0) == 0) setThemeByName(s.substr(6));
        else if (s.rfind("SOUND=", 0) == 0) setSoundByName(s.substr(6));
        else if (s.rfind("TIMEOUT=", 0) == 0) setTimeoutByName(s.substr(8));
        else if (s.rfind("POWER=", 0) == 0) power_save = s.substr(6) == "1";
        else if (s.rfind("CAST_HOST=", 0) == 0) setCastHost(s.substr(10));
        else if (s.rfind("CAST_PORT=", 0) == 0) setCastPortFromName(s.substr(10));
        else if (s.rfind("CAST_TRACE=", 0) == 0) cast_trace_enabled = s.substr(11) == "1";
    }
    fclose(f);
    applyPowerSavePreset();
    config_status = "LOADED";
}

bool writeReaderState()
{
    if (!initVoiceStorage()) return false;
    FILE* f = fopen(READER_STATE_TMP_FILE, "wb");
    if (!f) return false;
    if (!last_reader_book.empty()) fprintf(f, "LAST|%s\n", last_reader_book.c_str());
    for (const auto& item : reader_bookmarks) {
        fprintf(f, "BMK|%s|%d\n", item.first.c_str(), item.second);
    }
    for (const auto& item : reader_stream_bookmarks) {
        fprintf(f, "SBK2|%s|%lu\n", item.first.c_str(), static_cast<unsigned long>(item.second));
    }
    if (!flushAndClose(f)) {
        std::remove(READER_STATE_TMP_FILE);
        return false;
    }
    if (std::remove(READER_STATE_FILE) != 0 && errno != ENOENT) {
        std::remove(READER_STATE_TMP_FILE);
        return false;
    }
    if (std::rename(READER_STATE_TMP_FILE, READER_STATE_FILE) != 0) {
        std::remove(READER_STATE_TMP_FILE);
        return false;
    }
    return true;
}

void saveReaderState()
{
    // Queue only. The main loop writes after the streaming SD file is closed.
    reader_state_save_requested = true;
}

void loadReaderState()
{
    reader_bookmarks.clear();
    reader_stream_bookmarks.clear();
    last_reader_book.clear();
    if (!initVoiceStorage()) return;
    FILE* f = fopen(READER_STATE_FILE, "rb");
    bool loaded_legacy = false;
    if (!f && initSd()) {
        f = fopen(READER_STATE_LEGACY_FILE, "rb");
        loaded_legacy = f != nullptr;
    }
    if (!f) return;
    char line[180];
    while (fgets(line, sizeof(line), f)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.rfind("LAST|", 0) == 0) {
            last_reader_book = s.substr(5);
        } else if (s.rfind("BMK|", 0) == 0) {
            size_t p = s.rfind('|');
            if (p != std::string::npos && p > 4) {
                std::string name = s.substr(4, p - 4);
                int line_no = std::max(0, std::atoi(s.substr(p + 1).c_str()));
                if (!name.empty() && reader_bookmarks.size() < MAX_STATE_RECORDS) reader_bookmarks[name] = line_no;
            }
        } else if (s.rfind("SBK2|", 0) == 0) {
            size_t p = s.rfind('|');
            if (p != std::string::npos && p > 5) {
                std::string name = s.substr(5, p - 5);
                size_t offset = static_cast<size_t>(std::strtoul(s.substr(p + 1).c_str(), nullptr, 10));
                if (!name.empty() && reader_stream_bookmarks.size() < MAX_STATE_RECORDS) reader_stream_bookmarks[name] = offset;
            }
        }
    }
    fclose(f);
    if (loaded_legacy) reader_state_save_requested = true;
}

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
    if (!strcmp(name, "2") || !strcmp(name, "@")) return Key::Two;
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
    M5.Display.fillScreen(0x0000);
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
        // Keep enough VFS descriptors for a music stream plus directory and
        // metadata operations from the other offline apps.
        .max_files = 16,
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

DIR* openSdDirWithRetry(const char* path)
{
    if (!initSd()) return nullptr;
    DIR* dir = opendir(path);
    if (dir) return dir;
    // Do not unmount/remount on a single opendir failure: on Cardputer ADV that
    // can turn a transient SD/VFS miss into a global "No SD" state. Retry once
    // non-destructively and let the caller show an empty/error state if needed.
    vTaskDelay(pdMS_TO_TICKS(20));
    return opendir(path);
}

bool ensureSdDir(const char* path, std::string* err = nullptr)
{
    if (!initSd()) {
        if (err) *err = "sd mount";
        return false;
    }
    DIR* existing = opendir(path);
    if (existing) {
        closedir(existing);
        return true;
    }
    struct stat st = {};
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return true;

    for (int attempt = 0; attempt < 2; ++attempt) {
        errno = 0;
        if (mkdir(path, 0775) == 0 || errno == EEXIST) return true;
        const int saved_errno = errno;
        if (attempt == 0) {
            // A generic mkdir helper must not unmount/remount the shared SD
            // filesystem. Reprobe is an explicit Settings action only; doing
            // it here can invalidate handles owned by Music/Reader/Record.
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (err) {
            *err = "mkdir ";
            *err += std::to_string(saved_errno);
            *err += " ";
            *err += std::strerror(saved_errno);
        }
        return false;
    }
    return true;
}

bool isHidden(const std::string& name)
{
    if (name.empty() || name[0] == '.' || name.rfind("._", 0) == 0 || name == ".DS_Store") return true;
    // FATFS without long filename support exposes macOS AppleDouble "._FILE.EXT"
    // sidecars as short aliases like "_FILE_~1.TXT". Hide that pattern from all
    // user-facing SD lists while still allowing deliberate normal "_NAME" files.
    return name[0] == '_' && (name.find('~') != std::string::npos || name.size() >= 8);
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

bool hasRecordingExt(const std::string& name)
{
    const std::string ext = lowerExt(name);
    return ext == ".wav" || ext == ".pcm";
}

bool sdPathIsDir(const std::string& path)
{
    struct stat st = {};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string sdPathToFatPath(const std::string& path)
{
    if (path == MOUNT_POINT) return "0:/";
    if (path.rfind(MOUNT_POINT, 0) == 0) return "0:" + path.substr(std::strlen(MOUNT_POINT));
    return path;
}

std::string baseName(const std::string& path)
{
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string marqueeText(const std::string& text, size_t width)
{
    if (width == 0) return "";
    if (text.size() <= width) return text;
    std::string loop = text + "   ";
    const size_t off = (M5.millis() / 180) % loop.size();
    std::string out;
    out.reserve(width);
    for (size_t i = 0; i < width; ++i) {
        out.push_back(loop[(off + i) % loop.size()]);
    }
    return out;
}

std::string fileTypeLabel(const FileEntry& e)
{
    if (e.is_dir) return "DIR";
    const std::string ext = lowerExt(e.name);
    if (ext == ".mp3") return "MP3 audio";
    if (ext == ".txt") return "TXT text";
    if (ext == ".wav") return "WAV audio";
    if (ext == ".pcm") return "PCM audio";
    return "UNSUPPORTED";
}

std::string filesDisplayPath(const std::string& path)
{
    if (path == MOUNT_POINT) return "ROOT /";
    if (path == CONFIG_DIR) return "/TRANSFER";
    if (path.rfind(MOUNT_POINT, 0) == 0) {
        std::string rel = path.substr(std::strlen(MOUNT_POINT));
        if (rel == "/CARDPTR") return "/TRANSFER";
        if (rel.rfind("/CARDPTR/", 0) == 0) return "/TRANSFER" + rel.substr(std::strlen("/CARDPTR"));
        return rel.empty() ? "ROOT /" : rel;
    }
    return path;
}

bool isSupportedOpenFile(const FileEntry& e)
{
    return !e.is_dir && (hasMp3Ext(e.name) || hasTextExt(e.name) || hasRecordingExt(e.name));
}

bool createUnsupportedTestFile(std::string* err = nullptr)
{
    struct stat st = {};
    if (stat(CONFIG_DIR, &st) != 0) {
        if (mkdir(CONFIG_DIR, 0775) != 0 && errno != EEXIST) {
            if (err) {
                *err = "mkdir: ";
                *err += std::strerror(errno);
            }
            return false;
        }
    } else if (!S_ISDIR(st.st_mode)) {
        if (err) *err = "CARDPTR not dir";
        return false;
    }
    const std::string path = std::string(CONFIG_DIR) + "/X.BIN";
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        if (err) {
            *err = "open: ";
            *err += std::strerror(errno);
        }
        return false;
    }
    static constexpr const char kBody[] = "ABVx unsupported file test\n";
    const size_t want = sizeof(kBody) - 1;
    const size_t wrote = fwrite(kBody, 1, want, f);
    bool ok = wrote == want;
    if (fflush(f) != 0) ok = false;
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        if (err) {
            *err = "write: ";
            *err += errno ? std::strerror(errno) : "short write";
        }
        unlink(path.c_str());
        return false;
    }
    return true;
}

void scanFiles(const std::string& path)
{
    file_entries.clear();
    files_path = path.empty() ? std::string(MOUNT_POINT) : path;
    files_cursor = 0;
    files_status = "EMPTY";
    if (files_path == MOUNT_POINT) {
        // Hardware-safe root: avoid depending on root directory reads, which
        // have been flaky while direct known-folder reads stay reliable.
        file_entries.push_back({"BOOKS", BOOKS_DIR, true, 0});
        file_entries.push_back({"MUSIC", MUSIC_DIR, true, 0});
        file_entries.push_back({"NOTES", NOTES_DIR, true, 0});
        file_entries.push_back({"VOICE", recordings_dir, true, 0});
        file_entries.push_back({"TRANSFER", CONFIG_DIR, true, 0});
        files_status = "VIRTUAL";
        return;
    }
    if (files_path != MOUNT_POINT) {
        file_entries.push_back({"..", "", true, 0});
    }
    FF_DIR fat_dir = {};
    FILINFO info = {};
    if (f_opendir(&fat_dir, sdPathToFatPath(files_path).c_str()) == FR_OK) {
        while (f_readdir(&fat_dir, &info) == FR_OK && info.fname[0]) {
            std::string name = info.fname;
            if (isHidden(name)) continue;
            if (files_path == MOUNT_POINT && name == "CARDPTR") {
                file_entries.push_back({"TRANSFER", CONFIG_DIR, true, 0});
                continue;
            }
            std::string full = files_path + "/" + name;
            file_entries.push_back({name, full, (info.fattrib & AM_DIR) != 0, static_cast<size_t>(info.fsize)});
            if (file_entries.size() >= MAX_LIST_ENTRIES) break;
        }
        f_closedir(&fat_dir);
        files_status = "FAT OK";
    } else {
        DIR* dir = openSdDirWithRetry(files_path.c_str());
        if (!dir) {
            if (files_path == MOUNT_POINT) files_status = "NO SD";
        } else {
            while (dirent* entry = readdir(dir)) {
                std::string name = entry->d_name;
                if (isHidden(name)) continue;
                if (files_path == MOUNT_POINT && name == "CARDPTR") {
                    file_entries.push_back({"TRANSFER", CONFIG_DIR, true, 0});
                    continue;
                }
                std::string full = files_path + "/" + name;
                struct stat st = {};
                if (stat(full.c_str(), &st) != 0) continue;
                bool dir_flag = S_ISDIR(st.st_mode) || entry->d_type == DT_DIR;
                file_entries.push_back({name, full, dir_flag, static_cast<size_t>(st.st_size)});
                if (file_entries.size() >= MAX_LIST_ENTRIES) break;
            }
            closedir(dir);
        }
    }
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

void putLe16(uint8_t* p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

void putLe32(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

void buildWavHeader(uint8_t* h, uint32_t samples, uint32_t sample_rate, uint16_t bits)
{
    std::memset(h, 0, 44);
    const uint16_t bytes_per_sample = bits / 8;
    const uint32_t data_bytes = samples * bytes_per_sample;
    const uint32_t byte_rate = sample_rate * bytes_per_sample;
    std::memcpy(h, "RIFF", 4);
    putLe32(h + 4, 36 + data_bytes);
    std::memcpy(h + 8, "WAVEfmt ", 8);
    putLe32(h + 16, 16);
    putLe16(h + 20, 1);
    putLe16(h + 22, 1);
    putLe32(h + 24, sample_rate);
    putLe32(h + 28, byte_rate);
    putLe16(h + 32, bytes_per_sample);
    putLe16(h + 34, bits);
    std::memcpy(h + 36, "data", 4);
    putLe32(h + 40, data_bytes);
}

bool readWavHeader(FILE* f, uint32_t* data_offset, uint32_t* sample_rate,
                   uint16_t* bits_per_sample, std::string* err = nullptr)
{
    uint8_t h[44] = {};
    fseek(f, 0, SEEK_SET);
    if (fread(h, 1, sizeof(h), f) != sizeof(h)) {
        if (err) *err = "bad wav header";
        return false;
    }
    if (std::memcmp(h, "RIFF", 4) != 0 || std::memcmp(h + 8, "WAVE", 4) != 0 ||
        std::memcmp(h + 12, "fmt ", 4) != 0 || std::memcmp(h + 36, "data", 4) != 0) {
        if (err) *err = "unsupported wav";
        return false;
    }
    const uint16_t audio_format = h[20] | (h[21] << 8);
    const uint16_t channels = h[22] | (h[23] << 8);
    const uint32_t rate = h[24] | (h[25] << 8) | (h[26] << 16) | (h[27] << 24);
    const uint16_t bits = h[34] | (h[35] << 8);
    // Voice captures are intentionally compact 4 kHz mono PCM.  Keep the
    // generic WAV reader compatible with those internal Voice recordings.
    if (audio_format != 1 || channels != 1 || (bits != 8 && bits != 16) || rate < 4000 || rate > 48000) {
        if (err) *err = "wav must be pcm mono 8/16";
        return false;
    }
    if (data_offset) *data_offset = 44;
    if (sample_rate) *sample_rate = rate;
    if (bits_per_sample) *bits_per_sample = bits;
    return true;
}

std::string newRecordingNameFromMillis()
{
    char buf[16];
    snprintf(buf, sizeof(buf), "REC%05lu.WAV", static_cast<unsigned long>((M5.millis() / 100) % 100000));
    return buf;
}


void scanTextDir(const char* vfs_path, const char* fat_path, std::vector<std::string>* out)
{
    auto collect_posix = [&](DIR* dir) {
        int raw = 0;
        errno = 0;
        while (dirent* entry = readdir(dir)) {
            ++raw;
            std::string name = entry->d_name;
            if (isHidden(name) || !hasTextExt(name)) continue;
            if (sdPathIsDir(std::string(vfs_path) + "/" + name)) continue;
            out->push_back(name);
            if (out->size() >= MAX_LIST_ENTRIES) break;
        }
        closedir(dir);
        return raw;
    };

    out->clear();
    DIR* dir = openSdDirWithRetry(vfs_path);
    if (dir) collect_posix(dir);
    if (!out->empty()) return;

    // Bypass the POSIX VFS adapter as a final read-only fallback.
    FF_DIR fat_dir = {};
    FILINFO info = {};
    if (f_opendir(&fat_dir, fat_path) == FR_OK) {
        while (f_readdir(&fat_dir, &info) == FR_OK && info.fname[0]) {
            std::string name = info.fname;
            if (isHidden(name) || !hasTextExt(name)) continue;
            if (info.fattrib & AM_DIR) continue;
            out->push_back(name);
            if (out->size() >= MAX_LIST_ENTRIES) break;
        }
        f_closedir(&fat_dir);
    }
}

void scanBooks()
{
    scanTextDir(BOOKS_DIR, "0:/books", &books);
    std::sort(books.begin(), books.end());
    if (selected_book >= static_cast<int>(books.size())) selected_book = std::max(0, static_cast<int>(books.size()) - 1);
    if (!last_reader_book.empty()) {
        auto it = std::find(books.begin(), books.end(), last_reader_book);
        if (it != books.end()) selected_book = static_cast<int>(std::distance(books.begin(), it));
    }
}

std::string selectedBookPath()
{
    if (books.empty()) return "";
    return std::string(BOOKS_DIR) + "/" + books[selected_book];
}


bool ensureNotesDir(std::string* err = nullptr)
{
    return ensureSdDir(NOTES_DIR, err);
}

std::string inboxSafeText(std::string text)
{
    for (char& c : text) {
        if (static_cast<unsigned char>(c) < 32 || c == '|') c = ' ';
    }
    if (text.size() > 40) text.resize(40);
    return text;
}

void appendInboxEvent(const char* type, const std::string& detail)
{
    if (!type || !type[0]) return;
    inbox_pending_events.push_back("S" + std::to_string(M5.millis() / 1000) + "|" + type + "|" + inboxSafeText(detail));
    if (inbox_pending_events.size() > 16) inbox_pending_events.erase(inbox_pending_events.begin());
    inbox_persist_requested = true;
}

struct InboxEventView {
    std::string stamp;
    std::string type;
    std::string detail;
};

InboxEventView parseInboxEvent(const std::string& raw)
{
    InboxEventView out;
    const size_t first = raw.find('|');
    const size_t second = first == std::string::npos ? std::string::npos : raw.find('|', first + 1);
    if (first == std::string::npos || second == std::string::npos) {
        out.type = "EVENT";
        out.detail = raw;
        return out;
    }
    out.stamp = raw.substr(0, first);
    out.type = raw.substr(first + 1, second - first - 1);
    out.detail = raw.substr(second + 1);
    if (out.type.empty()) out.type = "EVENT";
    if (out.detail.empty()) out.detail = "-";
    return out;
}

void setInboxView(std::vector<std::string> events)
{
    inbox_entries = std::move(events);
    std::reverse(inbox_entries.begin(), inbox_entries.end());
    if (inbox_entries.empty()) inbox_cursor = 0;
    else inbox_cursor = std::max(0, std::min(inbox_cursor, static_cast<int>(inbox_entries.size()) - 1));
}

void refreshInboxManual()
{
    inbox_status = "QUEUED";
    inbox_persist_requested = true;
    dirty = true;
}

void processPersistence()
{
    // Internal flash writes are allowed only from the main loop on idle UI
    // screens. App handlers and all audio paths remain write-free.
    const bool safe_screen = screen == Screen::Launcher || screen == Screen::ReaderList ||
                             screen == Screen::InboxList || screen == Screen::InboxDetail;
    if (!safe_screen) return;
    if (reader_state_save_requested) {
        reader_state_save_requested = false;
        if (!writeReaderState()) config_status = "READER STATE ERR";
    }
    if (!inbox_persist_requested) return;
    inbox_persist_requested = false;
    inbox_flushed_last = 0;

    std::vector<std::string> stored;
    std::string err;
    bool ok = initVoiceStorage();
    if (!ok) {
        err = "mount";
    } else if (inbox_pending_events.empty()) {
        ok = abvx::loadEventLog(INBOX_FILE, &stored, &err);
    } else {
        const size_t pending_count = inbox_pending_events.size();
        ok = abvx::mergeEventLog(INBOX_FILE, INBOX_TMP_FILE, inbox_pending_events, &stored, &err);
        if (ok) {
            inbox_flushed_last = static_cast<int>(pending_count);
            inbox_pending_events.clear();
        }
    }

    if (!ok) {
        if (stored.empty() && initVoiceStorage()) abvx::loadEventLog(INBOX_FILE, &stored, nullptr);
        stored.insert(stored.end(), inbox_pending_events.begin(), inbox_pending_events.end());
        if (stored.size() > abvx::kEventLogMaxEntries) {
            stored.erase(stored.begin(), stored.end() - abvx::kEventLogMaxEntries);
        }
        inbox_status = "ERR " + inboxSafeText(err);
    } else {
        inbox_status = inbox_flushed_last > 0 ? "SAVED " + std::to_string(inbox_flushed_last) : "INTERNAL OK";
    }
    setInboxView(std::move(stored));
    dirty = true;
}

void scanNotes()
{
    scanTextDir(NOTES_DIR, "0:/notes", &notes);
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
    return "";
}

std::string habitDayId()
{
    char buf[12];
    snprintf(buf, sizeof(buf), "DAY%04d", habit_day);
    return buf;
}

bool ensureHabitsDir(std::string* err = nullptr)
{
    return ensureSdDir(HABITS_DIR, err);
}

bool ensureDefaultHabits(std::string* err = nullptr)
{
    if (!ensureHabitsDir(err)) return false;
    struct stat st = {};
    if (stat(HABITS_FILE, &st) == 0) return true;
    FILE* f = fopen(HABITS_FILE, "wb");
    if (!f) {
        if (err) {
            *err = "open ";
            *err += std::strerror(errno);
        }
        return false;
    }
    fputs("MEDS|Take pills|1\n", f);
    fputs("WALK|Walk|1\n", f);
    fputs("READ|Read|1\n", f);
    return flushAndClose(f);
}

void loadHabitState()
{
    if (!ensureHabitsDir()) return;
    FILE* f = fopen(HABIT_STATE_FILE, "rb");
    if (!f) return;
    int day = 0;
    if (fscanf(f, "%d", &day) == 1 && day >= 1 && day <= 9999) habit_day = day;
    fclose(f);
}

void saveHabitState()
{
    if (!ensureHabitsDir()) return;
    FILE* f = fopen(HABIT_STATE_FILE, "wb");
    if (!f) return;
    fprintf(f, "%d\n", habit_day);
    flushAndClose(f);
}

void loadHabitLogForDay()
{
    std::string day = habitDayId();
    FILE* f = fopen(HABIT_LOG_FILE, "rb");
    if (!f) return;
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        size_t p1 = s.find('|');
        size_t p2 = p1 == std::string::npos ? std::string::npos : s.find('|', p1 + 1);
        if (p1 == std::string::npos || p2 == std::string::npos) continue;
        if (s.substr(0, p1) != day) continue;
        std::string id = s.substr(p1 + 1, p2 - p1 - 1);
        bool done = s.substr(p2 + 1) == "1";
        for (auto& h : habits) {
            if (h.id == id) h.done = done;
        }
    }
    fclose(f);
}

void saveHabitLogForDay()
{
    if (!ensureHabitsDir()) return;
    std::string day = habitDayId();
    unlink(HABIT_LOG_TMP_FILE);
    FILE* out = fopen(HABIT_LOG_TMP_FILE, "wb");
    if (!out) return;
    bool ok = true;
    FILE* in = fopen(HABIT_LOG_FILE, "rb");
    if (in) {
        char line[160];
        while (fgets(line, sizeof(line), in)) {
            std::string s = line;
            size_t p = s.find('|');
            if (p != std::string::npos && s.substr(0, p) == day) continue;
            if (fputs(line, out) == EOF) { ok = false; break; }
        }
        fclose(in);
    }
    for (const auto& h : habits) {
        if (!h.active) continue;
        if (fprintf(out, "%s|%s|%d\n", day.c_str(), h.id.c_str(), h.done ? 1 : 0) < 0) ok = false;
    }
    if (!flushAndClose(out)) ok = false;
    if (!ok) {
        unlink(HABIT_LOG_TMP_FILE);
        return;
    }
    if (rename(HABIT_LOG_TMP_FILE, HABIT_LOG_FILE) != 0) {
        // FATFS may not replace an existing target atomically.
        unlink(HABIT_LOG_FILE);
        if (rename(HABIT_LOG_TMP_FILE, HABIT_LOG_FILE) != 0) unlink(HABIT_LOG_TMP_FILE);
    }
}

void scanHabits()
{
    habits.clear();
    disabled_habit_count = 0;
    std::string err;
    if (!ensureDefaultHabits(&err)) return;
    loadHabitState();
    FILE* f = fopen(HABITS_FILE, "rb");
    if (!f) return;
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        size_t p1 = s.find('|');
        size_t p2 = p1 == std::string::npos ? std::string::npos : s.find('|', p1 + 1);
        if (p1 == std::string::npos || p2 == std::string::npos) continue;
        Habit h;
        h.id = s.substr(0, p1);
        h.title = s.substr(p1 + 1, p2 - p1 - 1);
        h.active = s.substr(p2 + 1) != "0";
        h.done = false;
        if (h.active && !h.id.empty() && !h.title.empty() && habits.size() < MAX_HABITS) habits.push_back(h);
        else if (!h.active && !h.id.empty() && !h.title.empty()) ++disabled_habit_count;
    }
    fclose(f);
    loadHabitLogForDay();
    habits_cursor = std::max(0, std::min(habits_cursor, std::max(0, static_cast<int>(habits.size()) - 1)));
}

std::string nextHabitId()
{
    bool used[1000] = {};
    FILE* f = fopen(HABITS_FILE, "rb");
    if (f) {
        char line[160];
        while (fgets(line, sizeof(line), f)) {
            std::string s = line;
            size_t p = s.find('|');
            if (p == std::string::npos) continue;
            std::string id = s.substr(0, p);
            if (id.size() == 4 && id[0] == 'H') {
                int n = std::atoi(id.substr(1).c_str());
                if (n >= 0 && n < 1000) used[n] = true;
            }
        }
        fclose(f);
    }
    for (int i = 1; i < 1000; ++i) {
        if (!used[i]) {
            char buf[8];
            snprintf(buf, sizeof(buf), "H%03d", i);
            return buf;
        }
    }
    return "H999";
}

bool appendHabit(const std::string& title, std::string* err = nullptr)
{
    if (!ensureDefaultHabits(err)) return false;
    FILE* f = fopen(HABITS_FILE, "ab");
    if (!f) {
        if (err) {
            *err = "open ";
            *err += std::strerror(errno);
        }
        return false;
    }
    std::string clean = title.substr(0, 32);
    for (char& c : clean) {
        if (c == '|' || static_cast<unsigned char>(c) < 32) c = ' ';
    }
    fprintf(f, "%s|%s|1\n", nextHabitId().c_str(), clean.c_str());
    return flushAndClose(f);
}

bool renameHabit(const std::string& id, const std::string& title, std::string* err = nullptr)
{
    if (!ensureDefaultHabits(err)) return false;
    std::string clean = title.substr(0, 32);
    for (char& c : clean) {
        if (c == '|' || static_cast<unsigned char>(c) < 32) c = ' ';
    }
    if (clean.empty()) {
        if (err) *err = "empty title";
        return false;
    }
    FILE* in = fopen(HABITS_FILE, "rb");
    if (!in) {
        if (err) *err = "open";
        return false;
    }
    const std::string tmp_path = std::string(HABITS_DIR) + "/HABITS.NEW";
    unlink(tmp_path.c_str());
    FILE* out = fopen(tmp_path.c_str(), "wb");
    if (!out) {
        fclose(in);
        if (err) *err = "write open";
        return false;
    }
    bool found = false;
    bool ok = true;
    char line[160];
    while (fgets(line, sizeof(line), in)) {
        std::string s = line;
        size_t p1 = s.find('|');
        size_t p2 = p1 == std::string::npos ? std::string::npos : s.find('|', p1 + 1);
        if (p1 != std::string::npos && p2 != std::string::npos && s.substr(0, p1) == id) {
            const std::string active = s.substr(p2 + 1);
            if (fprintf(out, "%s|%s|%s", id.c_str(), clean.c_str(), active.c_str()) < 0) ok = false;
            found = true;
        } else if (fputs(line, out) == EOF) {
            ok = false;
        }
        if (!ok) break;
    }
    fclose(in);
    if (!flushAndClose(out)) ok = false;
    if (!ok || !found) {
        unlink(tmp_path.c_str());
        if (err) *err = found ? "write" : "not found";
        return false;
    }
    if (rename(tmp_path.c_str(), HABITS_FILE) != 0) {
        unlink(HABITS_FILE);
        if (rename(tmp_path.c_str(), HABITS_FILE) != 0) {
            unlink(tmp_path.c_str());
            if (err) *err = "replace";
            return false;
        }
    }
    return true;
}

bool disableHabit(const std::string& id, std::string* err = nullptr)
{
    if (!ensureDefaultHabits(err)) return false;
    std::vector<std::string> lines;
    FILE* in = fopen(HABITS_FILE, "rb");
    if (!in) return false;
    char line[160];
    while (fgets(line, sizeof(line), in)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        size_t p1 = s.find('|');
        size_t p2 = p1 == std::string::npos ? std::string::npos : s.find('|', p1 + 1);
        if (p1 != std::string::npos && p2 != std::string::npos && s.substr(0, p1) == id) {
            s = s.substr(0, p2 + 1) + "0";
        }
        lines.push_back(s + "\n");
    }
    fclose(in);
    FILE* out = fopen(HABITS_FILE, "wb");
    if (!out) {
        if (err) {
            *err = "open ";
            *err += std::strerror(errno);
        }
        return false;
    }
    for (const auto& s : lines) fputs(s.c_str(), out);
    return flushAndClose(out);
}

bool restoreAllHabits(std::string* err = nullptr)
{
    if (!ensureDefaultHabits(err)) return false;
    FILE* in = fopen(HABITS_FILE, "rb");
    if (!in) {
        if (err) *err = "open";
        return false;
    }
    const std::string tmp_path = std::string(HABITS_DIR) + "/HABITS.NEW";
    unlink(tmp_path.c_str());
    FILE* out = fopen(tmp_path.c_str(), "wb");
    if (!out) {
        fclose(in);
        if (err) *err = "write open";
        return false;
    }
    bool ok = true;
    char line[160];
    while (fgets(line, sizeof(line), in)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        size_t p1 = s.find('|');
        size_t p2 = p1 == std::string::npos ? std::string::npos : s.find('|', p1 + 1);
        if (p1 != std::string::npos && p2 != std::string::npos) {
            s = s.substr(0, p2 + 1) + "1";
        }
        if (fputs((s + "\n").c_str(), out) == EOF) {
            ok = false;
            break;
        }
    }
    fclose(in);
    if (!flushAndClose(out)) ok = false;
    if (!ok) {
        unlink(tmp_path.c_str());
        if (err) *err = "write";
        return false;
    }
    if (rename(tmp_path.c_str(), HABITS_FILE) != 0) {
        unlink(HABITS_FILE);
        if (rename(tmp_path.c_str(), HABITS_FILE) != 0) {
            unlink(tmp_path.c_str());
            if (err) *err = "replace";
            return false;
        }
    }
    return true;
}

int habitDoneCount(const std::string& id, int start_day, int end_day)
{
    FILE* f = fopen(HABIT_LOG_FILE, "rb");
    if (!f) return 0;
    int done_count = 0;
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        size_t p1 = s.find('|');
        size_t p2 = p1 == std::string::npos ? std::string::npos : s.find('|', p1 + 1);
        if (p1 == std::string::npos || p2 == std::string::npos) continue;
        std::string day = s.substr(0, p1);
        if (day.size() != 7 || day.rfind("DAY", 0) != 0) continue;
        int day_num = std::atoi(day.substr(3).c_str());
        if (day_num < start_day || day_num > end_day) continue;
        if (s.substr(p1 + 1, p2 - p1 - 1) == id && s.substr(p2 + 1) == "1") ++done_count;
    }
    fclose(f);
    return done_count;
}

int habitStreak(const std::string& id)
{
    constexpr int kStreakWindow = 365;
    const int start_day = std::max(1, habit_day - kStreakWindow + 1);
    std::vector<uint8_t> done(static_cast<size_t>(habit_day - start_day + 1), 0);
    FILE* f = fopen(HABIT_LOG_FILE, "rb");
    if (!f) return 0;
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        std::string s = line;
        size_t p1 = s.find('|');
        size_t p2 = p1 == std::string::npos ? std::string::npos : s.find('|', p1 + 1);
        if (p1 == std::string::npos || p2 == std::string::npos) continue;
        const std::string day = s.substr(0, p1);
        const int day_num = day.size() == 7 && day.rfind("DAY", 0) == 0 ? std::atoi(day.substr(3).c_str()) : 0;
        if (day_num < start_day || day_num > habit_day) continue;
        if (s.substr(p1 + 1, p2 - p1 - 1) == id && s.substr(p2 + 1) == "1") done[day_num - start_day] = 1;
    }
    fclose(f);
    int streak = 0;
    for (int day = habit_day; day >= start_day && done[day - start_day]; --day) ++streak;
    return streak;
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
    constexpr int max_cols = 17;
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

void closeReaderStream()
{
    if (reader_stream_file) {
        fclose(reader_stream_file);
        reader_stream_file = nullptr;
    }
}

bool readReaderStreamBytes(size_t offset, uint8_t* data, size_t capacity,
                           size_t* bytes_read, std::string* err = nullptr)
{
    int last_errno = EIO;
    const char* failed_stage = "read";
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!reader_stream_file) reader_stream_file = fopen(reader_stream_path.c_str(), "rb");
        if (!reader_stream_file) {
            last_errno = errno;
            failed_stage = "open";
        } else {
            clearerr(reader_stream_file);
            errno = 0;
            if (fseek(reader_stream_file, static_cast<long>(offset), SEEK_SET) != 0) {
                last_errno = errno ? errno : EIO;
                failed_stage = "seek";
            } else {
                const size_t count = fread(data, 1, capacity, reader_stream_file);
                if (count > 0) {
                    *bytes_read = count;
                    return true;
                }
                last_errno = errno ? errno : EIO;
                failed_stage = feof(reader_stream_file) ? "end" : "read";
            }
        }
        closeReaderStream();
        if (attempt == 0) vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (err) *err = std::string(failed_stage) + ": " + std::strerror(last_errno);
    return false;
}

// Large books are rendered as a small moving window.  Keeping only five wrapped
// lines avoids the text/word vectors that make a normal book exceed Cardputer RAM.
bool loadReaderStreamPage(std::string* err = nullptr)
{
    reader_stream_lines.clear();
    reader_stream_line_offsets.clear();
    if (reader_stream_path.empty() || reader_stream_offset >= reader_stream_file_size) {
        if (err) *err = "end of book";
        return false;
    }
    std::vector<uint8_t> bytes(READER_STREAM_READ_BYTES);
    size_t n = 0;
    if (!readReaderStreamBytes(reader_stream_offset, bytes.data(), bytes.size(), &n, err)) return false;

    constexpr int max_cols = 17;
    std::string line;
    std::string word;
    int line_cols = 0;
    int word_cols = 0;
    size_t line_start = reader_stream_offset;
    size_t word_start = reader_stream_offset;
    auto append_line = [&]() {
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) line.pop_back();
        reader_stream_line_offsets.push_back(line_start);
        reader_stream_lines.push_back(line.empty() ? " " : line);
        line.clear();
        line_cols = 0;
    };
    auto flush_word = [&]() {
        if (word.empty()) return;
        if (line_cols == 0) line_start = word_start;
        if (line_cols > 0 && line_cols + 1 + word_cols > max_cols) append_line();
        if (line_cols == 0) line_start = word_start;
        if (word_cols > max_cols) {
            if (!line.empty()) append_line();
            std::string part;
            int cols = 0;
            size_t part_start = word_start;
            for (size_t i = 0; i < word.size();) {
                size_t len = utf8CharLen(static_cast<unsigned char>(word[i]));
                if (i + len > word.size()) len = 1;
                if (cols >= max_cols) {
                    line_start = part_start;
                    line = part;
                    line_cols = cols;
                    append_line();
                    part.clear();
                    cols = 0;
                    part_start = word_start + i;
                }
                part.append(word, i, len);
                ++cols;
                i += len;
            }
            line_start = part_start;
            line = part;
            line_cols = cols;
        } else {
            if (line_cols > 0) { line += ' '; ++line_cols; }
            line += word;
            line_cols += word_cols;
        }
        word.clear();
        word_cols = 0;
    };

    for (size_t i = 0; i < n && reader_stream_lines.size() < READER_STREAM_LOOKAHEAD_LINES;) {
        unsigned char c = bytes[i];
        if (c == '\r') { ++i; continue; }
        if (c == '\n') {
            flush_word();
            if (!line.empty() || reader_stream_lines.empty() || line_start != reader_stream_offset + i) append_line();
            ++i;
            line_start = reader_stream_offset + i;
            continue;
        }
        if (c == ' ' || c == '\t') { flush_word(); ++i; continue; }
        size_t len = utf8CharLen(c);
        if (i + len > n) break; // Do not split a UTF-8 character at the window boundary.
        if (word.empty()) word_start = reader_stream_offset + i;
        word.append(reinterpret_cast<const char*>(bytes.data() + i), len);
        ++word_cols;
        i += len;
    }
    if (reader_stream_lines.size() < READER_STREAM_LOOKAHEAD_LINES) {
        flush_word();
        if (!line.empty()) append_line();
    }
    if (reader_stream_lines.empty()) {
        reader_stream_line_offsets.push_back(reader_stream_offset);
        reader_stream_lines.push_back(" ");
    }
    return true;
}

bool readerStreamMoveForward(int lines)
{
    bool moved = false;
    for (int i = 0; i < lines; ++i) {
        if (reader_stream_line_offsets.size() < 2) break;
        size_t next = reader_stream_line_offsets[1];
        if (next <= reader_stream_offset || next >= reader_stream_file_size) break;
        const size_t previous = reader_stream_offset;
        reader_stream_history.push_back(previous);
        reader_stream_offset = next;
        std::string read_error;
        if (!loadReaderStreamPage(&read_error)) {
            reader_stream_offset = previous;
            reader_stream_history.pop_back();
            loadReaderStreamPage(nullptr);
            reader_stream_status = "READ: " + (read_error.empty() ? std::string("failed") : read_error);
            break;
        }
        moved = true;
    }
    return moved;
}

bool readerStreamMoveBack(int lines)
{
    bool moved = false;
    for (int i = 0; i < lines && !reader_stream_history.empty(); ++i) {
        const size_t previous = reader_stream_offset;
        reader_stream_offset = reader_stream_history.back();
        reader_stream_history.pop_back();
        std::string read_error;
        if (!loadReaderStreamPage(&read_error)) {
            reader_stream_history.push_back(reader_stream_offset);
            reader_stream_offset = previous;
            loadReaderStreamPage(nullptr);
            reader_stream_status = "READ: " + (read_error.empty() ? std::string("failed") : read_error);
            break;
        }
        moved = true;
    }
    return moved;
}

bool readerStreamReadWord(size_t from, std::string* out, size_t* start_out, size_t* next_out, std::string* err = nullptr)
{
    if (from >= reader_stream_file_size) {
        if (err) *err = "end of book";
        return false;
    }
    uint8_t bytes[512];
    size_t n = 0;
    if (!readReaderStreamBytes(from, bytes, sizeof(bytes), &n, err)) return false;
    size_t i = 0;
    auto whitespace = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (i < n && whitespace(bytes[i])) ++i;
    if (i >= n) {
        if (err) *err = "end of book";
        return false;
    }
    const size_t start = from + i;
    std::string word;
    while (i < n && !whitespace(bytes[i])) {
        size_t len = utf8CharLen(bytes[i]);
        if (i + len > n) break;
        if (word.size() + len > 96) break;
        word.append(reinterpret_cast<const char*>(bytes + i), len);
        i += len;
    }
    if (word.empty()) {
        if (err) *err = "word too long";
        return false;
    }
    *out = word;
    *start_out = start;
    *next_out = from + i;
    return true;
}

bool loadReaderStreamSpeedUnit(std::string* err = nullptr)
{
    reader_stream_speed_text.clear();
    reader_stream_speed_next_offset = 0;
    if (speed_mode == SpeedMode::Line) {
        reader_stream_offset = reader_stream_speed_offset;
        if (!loadReaderStreamPage(err)) return false;
        reader_stream_speed_offset = reader_stream_offset;
        reader_stream_speed_text = reader_stream_lines.empty() ? " " : reader_stream_lines.front();
        if (reader_stream_line_offsets.size() > 1) reader_stream_speed_next_offset = reader_stream_line_offsets[1];
        return true;
    }

    std::string first;
    size_t first_start = 0;
    size_t first_next = 0;
    if (!readerStreamReadWord(reader_stream_speed_offset, &first, &first_start, &first_next, err)) return false;
    reader_stream_speed_offset = first_start;
    reader_stream_offset = first_start;
    reader_stream_speed_text = first;
    reader_stream_speed_next_offset = first_next;
    if (speed_mode == SpeedMode::TwoWords) {
        std::string second;
        size_t second_start = 0;
        size_t second_next = 0;
        std::string ignored;
        if (readerStreamReadWord(first_next, &second, &second_start, &second_next, &ignored)) {
            reader_stream_speed_text += " ";
            reader_stream_speed_text += second;
            reader_stream_speed_next_offset = second_next;
        } else {
            reader_stream_speed_next_offset = 0;
        }
    }
    return true;
}

bool loadTextFile(const std::string& path, const std::string& label, std::string* err = nullptr)
{
    closeReaderStream();
    reader_streaming = false;
    reader_stream_path.clear();
    reader_stream_lines.clear();
    reader_stream_line_offsets.clear();
    reader_stream_history.clear();
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
    if (active_book_name.empty()) return;
    last_reader_book = active_book_name;
    if (reader_streaming) {
        reader_stream_bookmarks[active_book_name] = reader_stream_offset;
        return;
    }
    if (reader_lines.empty()) return;
    reader_bookmarks[active_book_name] = clampReaderLine(currentReaderLineForBookmark());
}

bool loadSelectedBook(std::string* err = nullptr)
{
    if (books.empty()) {
        if (err) *err = "no books";
        return false;
    }
    const std::string path = selectedBookPath();
    closeReaderStream();
    reader_streaming = false;
    reader_stream_status.clear();
    struct stat st = {};
    if (stat(path.c_str(), &st) != 0 || st.st_size <= 0) {
        if (err) *err = st.st_size <= 0 ? "empty file" : std::string("stat: ") + std::strerror(errno);
        return false;
    }
    if (static_cast<size_t>(st.st_size) > MAX_BOOK_BYTES) {
        reader_streaming = true;
        reader_stream_path = path;
        reader_stream_file_size = static_cast<size_t>(st.st_size);
        reader_stream_offset = 0;
        reader_stream_history.clear();
        reader_stream_status.clear();
        reader_text.clear();
        reader_lines.clear();
        reader_words.clear();
        active_book_name = books[selected_book];
        auto stream_bmk = reader_stream_bookmarks.find(active_book_name);
        if (stream_bmk != reader_stream_bookmarks.end() && stream_bmk->second < reader_stream_file_size) {
            reader_stream_offset = stream_bmk->second;
        }
        if (!loadReaderStreamPage(err)) {
            closeReaderStream();
            reader_streaming = false;
            return false;
        }
    } else if (!loadTextFile(path, books[selected_book], err)) {
        return false;
    }
    last_reader_book = active_book_name;
    reader_opened_this_session = true;
    if (!reader_streaming) {
        auto it = reader_bookmarks.find(active_book_name);
        if (it != reader_bookmarks.end()) reader_scroll = clampReaderLine(it->second);
    }
    saveReaderState();
    appendInboxEvent("READ", active_book_name);
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

std::string noteTextForSave()
{
    return note_input;
}

bool containsCyrillic(const std::string& text);

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

std::vector<std::string> wrapUtf8TextColumns(const std::string& text, int max_cols)
{
    std::vector<std::string> lines;
    std::string line;
    std::string word;
    int line_cols = 0;
    int word_cols = 0;
    auto flush_word = [&]() {
        if (word.empty()) return;
        if (line_cols > 0 && line_cols + 1 + word_cols > max_cols) {
            lines.push_back(line);
            line.clear();
            line_cols = 0;
        }
        if (word_cols > max_cols) {
            if (!line.empty()) {
                lines.push_back(line);
                line.clear();
                line_cols = 0;
            }
            std::string part;
            int cols = 0;
            for (size_t i = 0; i < word.size();) {
                size_t len = utf8CharLen(static_cast<unsigned char>(word[i]));
                if (i + len > word.size()) len = 1;
                if (cols >= max_cols) {
                    lines.push_back(part);
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
        word.clear();
        word_cols = 0;
    };
    for (size_t i = 0; i < text.size();) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '\r') { ++i; continue; }
        if (c == '\n') {
            flush_word();
            lines.push_back(line.empty() ? " " : line);
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
        if (i + len > text.size()) len = 1;
        word.append(text, i, len);
        ++word_cols;
        i += len;
    }
    flush_word();
    if (!line.empty()) lines.push_back(line);
    if (lines.empty()) lines.push_back("");
    return lines;
}

bool saveNewNote(std::string* out_name, std::string* err = nullptr)
{
    if (!ensureNotesDir(err)) return false;
    std::string name = nextNoteName();
    if (name.empty()) {
        if (err) *err = "note namespace full";
        return false;
    }
    std::string path = std::string(NOTES_DIR) + "/" + name;
    for (int tries = 0; tries < 9999; ++tries) {
        struct stat st = {};
        if (stat(path.c_str(), &st) != 0) break;
        notes.push_back(name);
        name = nextNoteName();
        if (name.empty()) {
            if (err) *err = "note namespace full";
            return false;
        }
        path = std::string(NOTES_DIR) + "/" + name;
    }
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        if (err) {
            *err = "open ";
            *err += std::strerror(errno);
            *err += " ";
            *err += name;
        }
        return false;
    }
    std::string body = noteTextForSave();
    body += "\n";
    size_t n = fwrite(body.data(), 1, body.size(), f);
    bool ok = n == body.size();
    if (!flushAndClose(f)) ok = false;
    if (!ok) {
        if (err) {
            *err = "write ";
            *err += std::strerror(errno);
            *err += " ";
            *err += name;
        }
        unlink(path.c_str());
        return false;
    }
    if (out_name) *out_name = name;
    active_note_name = name;
    last_note_name = name;
    scanNotes();
    for (int i = 0; i < static_cast<int>(notes.size()); ++i) {
        if (notes[i] == name) {
            notes_cursor = i + 1;
            break;
        }
    }
    return true;
}

bool loadRawNoteForEdit(std::string* err = nullptr)
{
    std::string path = selectedNotePath();
    if (path.empty()) {
        if (err) *err = "no note";
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
    std::string body;
    char buf[128];
    while (size_t n = fread(buf, 1, sizeof(buf), f)) {
        if (body.size() + n > 512) {
            n = 512 - body.size();
        }
        body.append(buf, n);
        if (body.size() >= 512) break;
    }
    fclose(f);
    while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.pop_back();
    active_note_name = notes[notes_cursor - 1];
    if (containsCyrillic(body)) {
        if (err) *err = "Cyrillic view only";
        return false;
    }
    note_input = body;
    note_edit_existing = true;
    return true;
}

bool saveExistingNote(std::string* err = nullptr)
{
    std::string path = selectedNotePath();
    if (path.empty()) {
        if (err) *err = "no note";
        return false;
    }
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        if (err) {
            *err = "open ";
            *err += std::strerror(errno);
            *err += " ";
            *err += active_note_name;
        }
        return false;
    }
    std::string body = noteTextForSave();
    body += "\n";
    size_t n = fwrite(body.data(), 1, body.size(), f);
    bool ok = n == body.size();
    if (!flushAndClose(f)) ok = false;
    if (!ok) {
        if (err) {
            *err = "write ";
            *err += std::strerror(errno);
            *err += " ";
            *err += active_note_name;
        }
        return false;
    }
    last_note_name = active_note_name;
    scanNotes();
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

void loadMusicIndex()
{
    music_titles.clear();
    FILE* f = fopen(MUSIC_INDEX_FILE, "rb");
    if (!f) return;
    char line[192];
    while (fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        size_t sep = s.find('|');
        if (sep == std::string::npos || sep == 0 || sep + 1 >= s.size()) continue;
        music_titles[s.substr(0, sep)] = s.substr(sep + 1);
        if (music_titles.size() >= MAX_STATE_RECORDS) break;
    }
    fclose(f);
}

std::string musicDisplayName(const std::string& file)
{
    auto it = music_titles.find(file);
    if (it != music_titles.end() && !it->second.empty()) return it->second;
    return file;
}

bool musicStorageNameNeedsImport(const std::string& file)
{
    if (file.empty()) return true;
    for (unsigned char c : file) {
        // FATFS is configured for heap-backed LFN with an UTF-8 API. Non-ASCII
        // bytes are therefore valid path data; only structurally unsafe names
        // remain blocked from the playback path.
        if (c < 32 || c == 127 || c == '/' || c == '\\') return true;
    }
    return false;
}

void shuffleIndices(std::vector<int>* values)
{
    if (!values) return;
    for (size_t i = values->size(); i > 1; --i) {
        const size_t j = static_cast<size_t>(esp_random()) % i;
        std::swap((*values)[i - 1], (*values)[j]);
    }
}

void beginShuffleFromCurrent()
{
    shuffle_order.clear();
    shuffle_position = -1;
    if (tracks.empty()) return;
    selected_track = std::max(0, std::min(selected_track, static_cast<int>(tracks.size()) - 1));
    shuffle_order.reserve(tracks.size());
    shuffle_order.push_back(selected_track);
    std::vector<int> remaining;
    remaining.reserve(tracks.size() - 1);
    for (int i = 0; i < static_cast<int>(tracks.size()); ++i) {
        if (i != selected_track) remaining.push_back(i);
    }
    shuffleIndices(&remaining);
    shuffle_order.insert(shuffle_order.end(), remaining.begin(), remaining.end());
    shuffle_position = 0;
}

void beginNewShuffleCycle(int previous_track)
{
    shuffle_order.clear();
    shuffle_order.reserve(tracks.size());
    for (int i = 0; i < static_cast<int>(tracks.size()); ++i) shuffle_order.push_back(i);
    shuffleIndices(&shuffle_order);
    if (shuffle_order.size() > 1 && shuffle_order.front() == previous_track) {
        std::swap(shuffle_order.front(), shuffle_order[1]);
    }
    shuffle_position = shuffle_order.empty() ? -1 : 0;
}

void setShuffleEnabled(bool enabled)
{
    shuffle_on = enabled;
    if (shuffle_on) beginShuffleFromCurrent();
    else {
        shuffle_order.clear();
        shuffle_position = -1;
    }
}

void advanceTrackSelection(int delta)
{
    if (tracks.empty()) return;
    if (!shuffle_on || tracks.size() == 1) {
        selected_track = (selected_track + delta + tracks.size()) % tracks.size();
        return;
    }
    if (shuffle_order.size() != tracks.size() || shuffle_position < 0 ||
        shuffle_position >= static_cast<int>(shuffle_order.size()) ||
        shuffle_order[shuffle_position] != selected_track) {
        beginShuffleFromCurrent();
    }
    if (delta < 0) {
        if (shuffle_position > 0) --shuffle_position;
    } else if (shuffle_position + 1 < static_cast<int>(shuffle_order.size())) {
        ++shuffle_position;
    } else {
        beginNewShuffleCycle(selected_track);
    }
    if (shuffle_position >= 0) selected_track = shuffle_order[shuffle_position];
}

bool resolveMusicFile(const std::string& file, std::string* resolved_path, struct stat* out)
{
    if (file.empty() || !out) return false;
    const std::string alias_path = std::string(MUSIC_DIR) + "/" + file;
    if (stat(alias_path.c_str(), out) == 0 && S_ISREG(out->st_mode)) {
        if (resolved_path) *resolved_path = alias_path;
        return true;
    }
    const auto physical = music_physical_names.find(file);
    if (physical == music_physical_names.end() || physical->second.empty()) return false;
    const std::string lfn_path = std::string(MUSIC_DIR) + "/" + physical->second;
    if (stat(lfn_path.c_str(), out) != 0 || !S_ISREG(out->st_mode)) return false;
    if (resolved_path) *resolved_path = lfn_path;
    return true;
}

bool musicFileStat(const std::string& file, struct stat* out)
{
    return resolveMusicFile(file, nullptr, out);
}

FRESULT openFatMusicFile(const std::string& file, FIL* opened,
                         std::string* resolved_path, std::string* err)
{
    if (file.empty() || !opened) {
        if (err) *err = "empty filename";
        return FR_INVALID_NAME;
    }
    const std::string alias_fat_path = std::string("0:/music/") + file;
    FRESULT result = f_open(opened, alias_fat_path.c_str(), FA_READ);
    if (result == FR_OK) {
        if (resolved_path) *resolved_path = std::string(MUSIC_DIR) + "/" + file;
        return FR_OK;
    }
    const FRESULT first_result = result;
    const auto physical = music_physical_names.find(file);
    if (physical != music_physical_names.end() && !physical->second.empty()) {
        const std::string lfn_fat_path = std::string("0:/music/") + physical->second;
        result = f_open(opened, lfn_fat_path.c_str(), FA_READ);
        if (result == FR_OK) {
            if (resolved_path) *resolved_path = std::string(MUSIC_DIR) + "/" + physical->second;
            return FR_OK;
        }
    }
    if (err) {
        const FRESULT effective = result == FR_OK ? first_result : result;
        if (effective == FR_INVALID_NAME) {
            *err = "Unsupported filename";
        } else {
            *err = "fat open ";
            *err += std::to_string(static_cast<int>(effective));
        }
    }
    return result == FR_OK ? first_result : result;
}

std::string sortKey(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void scanMusic()
{
    music_scan_status = "FAT SCAN";
    music_raw_entries = 0;
    music_mp3_entries = 0;
    loadMusicIndex();
    music_physical_names.clear();
    std::vector<std::string> next_tracks;
    next_tracks.reserve(32);
    auto collectMusicDir = [&]() {
        FF_DIR fat_dir = {};
        FILINFO info = {};
        FRESULT result = f_opendir(&fat_dir, "0:/music");
        if (result != FR_OK) return result;
        while ((result = f_readdir(&fat_dir, &info)) == FR_OK && info.fname[0]) {
            ++music_raw_entries;
            const std::string display_name = info.fname;
            const std::string storage_name = info.altname[0] ? std::string(info.altname) : display_name;
            if (isHidden(display_name) || isHidden(storage_name)) continue;
            if (!hasMp3Ext(display_name) && !hasMp3Ext(storage_name)) continue;
            // macOS AppleDouble sidecars can lose their leading "._" when
            // exposed through a FAT short alias (for example WTHE~300.MP3).
            // They are commonly 4096 bytes and contain no MPEG frames.
            if ((info.fattrib & (AM_DIR | AM_HID | AM_SYS)) || info.fsize < 8192) continue;
            ++music_mp3_entries;
            // Keep only the stable FAT short alias in the playback path. The
            // long UTF-8 name is display metadata and never reaches stat/fopen.
            next_tracks.push_back(storage_name);
            if (display_name != storage_name) music_physical_names[storage_name] = display_name;
            if (display_name != storage_name && music_titles.find(storage_name) == music_titles.end()) {
                music_titles[storage_name] = display_name;
            }
            if (next_tracks.size() >= MAX_LIST_ENTRIES) break;
        }
        f_closedir(&fat_dir);
        return result;
    };

    FRESULT fatfs_result = collectMusicDir();
    if (fatfs_result != FR_OK || music_raw_entries == 0) {
        music_scan_status = "RETRY";
        if (manualSdReprobe()) {
            next_tracks.clear();
            music_raw_entries = 0;
            music_mp3_entries = 0;
            fatfs_result = collectMusicDir();
        }
    }
    std::sort(next_tracks.begin(), next_tracks.end(), [](const std::string& a, const std::string& b) {
        return sortKey(musicDisplayName(a)) < sortKey(musicDisplayName(b));
    });
    tracks.swap(next_tracks);
    shuffle_order.clear();
    shuffle_position = -1;
    if (fatfs_result != FR_OK) {
        music_scan_status = "FAT " + std::to_string(static_cast<int>(fatfs_result));
    } else {
        music_scan_status = "FAT OK";
    }
    if (selected_track >= static_cast<int>(tracks.size())) selected_track = std::max(0, static_cast<int>(tracks.size()) - 1);
}

void scanRecordings()
{
    recordings.clear();
    recordings_dir = VOICE_STORE_DIR;
    if (!initVoiceStorage()) return;
    DIR* dir = opendir(VOICE_STORE_DIR);
    if (!dir) return;
    while (dirent* entry = readdir(dir)) {
        std::string name = entry->d_name;
        if (isHidden(name) || !hasRecordingExt(name)) continue;
        if (sdPathIsDir(std::string(VOICE_STORE_DIR) + "/" + name)) continue;
        recordings.push_back(name);
        if (recordings.size() >= MAX_LIST_ENTRIES) break;
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

std::string musicProblemBody(const std::string& path, const std::string& err)
{
    struct stat st = {};
    std::string body = baseName(path);
    if (stat(path.c_str(), &st) == 0) {
        body += "\nSIZE ";
        body += std::to_string(static_cast<unsigned long long>(st.st_size));
        body += "B";
    }
    body += "\n";
    body += err.empty() ? "decode failed" : err;
    return body;
}

bool isValidMp3FrameHeader(const uint8_t* p);

void prepareMusicInfo()
{
    music_info_title.clear();
    music_info_file.clear();
    music_info_size = "-";
    music_info_status = "READY";
    music_info_index = tracks.empty() ? 0 : selected_track + 1;
    music_info_total = static_cast<int>(tracks.size());
    if (tracks.empty()) return;
    music_info_file = tracks[selected_track];
    music_info_title = musicDisplayName(music_info_file);
    struct stat st = {};
    if (musicFileStat(music_info_file, &st)) {
        music_info_size = std::to_string(static_cast<unsigned long>(st.st_size)) + " B";
        if (st.st_size <= 0) music_info_status = "BAD zero";
    } else {
        music_info_status = "MISSING";
    }
    if (musicStorageNameNeedsImport(music_info_file)) music_info_status = "IMPORT REQUIRED";
}

bool probeSelectedMusic(std::string* status)
{
    prepareMusicInfo();
    if (tracks.empty()) {
        if (status) *status = "BAD no track";
        return false;
    }
    std::string path;
    std::string open_error;
    FIL probe_file = {};
    if (openFatMusicFile(tracks[selected_track], &probe_file, &path, &open_error) != FR_OK) {
        if (status) *status = "BAD " + open_error;
        return false;
    }
    if (f_size(&probe_file) < 64) {
        f_close(&probe_file);
        if (status) *status = "BAD small";
        return false;
    }
    std::vector<uint8_t> probe(32768);
    UINT n = 0;
    const FRESULT read_result = f_read(&probe_file, probe.data(), probe.size(), &n);
    f_close(&probe_file);
    if (read_result != FR_OK) {
        if (status) *status = "BAD fat read " + std::to_string(static_cast<int>(read_result));
        return false;
    }
    if (n < 4) {
        if (status) *status = "BAD read";
        return false;
    }
    size_t sync = std::string::npos;
    for (size_t i = 0; i + 3 < n; ++i) {
        if (isValidMp3FrameHeader(probe.data() + i)) {
            sync = i;
            break;
        }
    }
    if (sync == std::string::npos) {
        if (status) *status = "BAD no sync";
        return false;
    }
    if (status) *status = "OK sync " + std::to_string(static_cast<unsigned long>(sync));
    return true;
}

bool hasMusicPlaybackHeap(std::string* err = nullptr)
{
    const size_t pcm_values = (AUDIO_OUTPUT_RATE * CHUNK_MS / 1000) + MINIMP3_MAX_SAMPLES_PER_FRAME;
    const auto allocationNeeded = [](size_t current_capacity, size_t required_capacity, size_t element_size) {
        return current_capacity >= required_capacity ? static_cast<size_t>(0) : required_capacity * element_size;
    };
    const size_t input_need = allocationNeeded(mp3_buf.capacity(), INPUT_BUF_SIZE, sizeof(uint8_t));
    const size_t frame_need = allocationNeeded(mp3_frame_pcm.capacity(), MINIMP3_MAX_SAMPLES_PER_FRAME, sizeof(mp3d_sample_t));
    const size_t current_need = allocationNeeded(pcm_chunk.capacity(), pcm_values, sizeof(int16_t));
    const size_t queued_need = allocationNeeded(pcm_next_chunk.capacity(), pcm_values, sizeof(int16_t));
    const size_t required_total = input_need + frame_need + current_need + queued_need + 8192;
    const size_t required_block = std::max(std::max(input_need, frame_need), std::max(current_need, queued_need));
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    const size_t free_bytes = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (largest >= required_block && free_bytes >= required_total) return true;
    if (err) {
        *err = "low heap ";
        *err += std::to_string(static_cast<unsigned long long>(largest));
        *err += "B free ";
        *err += std::to_string(static_cast<unsigned long long>(free_bytes));
        *err += "B";
    }
    return false;
}

uint32_t recSecondsMsFromSamples(uint32_t samples)
{
    return rec_record_sample_rate ? (samples * 1000ULL) / rec_record_sample_rate : 0;
}

int32_t recClockDeltaMs()
{
    const uint32_t clk_ms = (rec_stopped_ms >= rec_started_ms) ? (rec_stopped_ms - rec_started_ms) : ((M5.millis() >= rec_started_ms) ? (M5.millis() - rec_started_ms) : 0);
    const uint32_t pcm_ms = recSecondsMsFromSamples(rec_samples_written);
    return static_cast<int32_t>(clk_ms) - static_cast<int32_t>(pcm_ms);
}

uint32_t recElapsedMs()
{
    return rec_started_ms ? M5.millis() - rec_started_ms : 0;
}

bool recTimeReached()
{
    return rec_started_ms != 0 && recElapsedMs() + REC_STOP_GRACE_MS >= rec_target_ms;
}

void formatTenthsSeconds(uint32_t ms, char* out, size_t out_len)
{
    const uint32_t sec = ms / 1000;
    const uint32_t tenth = (ms % 1000) / 100;
    snprintf(out, out_len, "%lu.%lu", static_cast<unsigned long>(sec), static_cast<unsigned long>(tenth));
}

void applyVolume()
{
    int vol = 180;
    if (volume_mode == VolumeMode::Mute || sound_mode == SoundMode::Off) vol = 0;
    else if (volume_mode == VolumeMode::Loud) vol = 255;
    M5.Speaker.setVolume(vol);
}

const char* volumeName()
{
    if (volume_mode == VolumeMode::Mute) return "MUTE";
    if (volume_mode == VolumeMode::Loud) return "MAX";
    return "MID";
}

void stopPlayback()
{
    playing = false;
    music_autostart_pending = false;
    M5.Speaker.stop();
    if (mp3_fat_open) {
        f_close(&mp3_fat_file);
        mp3_fat_open = false;
    }
    mp3_len = 0;
    mp3_pos = 0;
    mp3_eof = false;
    decoded_chunks = 0;
    music_underruns = 0;
    pcm_chunk.clear();
    pcm_next_chunk.clear();
    pcm_next_ready = false;
    playback_decode_after_ms = 0;
}

template <typename T>
void releaseVector(std::vector<T>* values)
{
    if (!values) return;
    std::vector<T>().swap(*values);
}

void releaseTransientBuffersForVoice()
{
    // clear() retains capacity. Reader and Music could therefore leave the
    // heap too fragmented for the bounded 20-second Voice capture pool.
    stopPlayback();
    closeReaderStream();
    if (rec_play_file) {
        fclose(rec_play_file);
        rec_play_file = nullptr;
    }
    M5.Speaker.stop();

    releaseVector(&mp3_buf);
    releaseVector(&mp3_frame_pcm);
    releaseVector(&pcm_chunk);
    releaseVector(&pcm_next_chunk);
    std::string().swap(reader_text);
    releaseVector(&reader_lines);
    releaseVector(&reader_words);
    releaseVector(&reader_stream_lines);
    releaseVector(&reader_stream_line_offsets);
    releaseVector(&reader_stream_history);
    releaseVector(&rec_buffer);
    releaseVector(&rec_u8_buffer);
    releaseVector(&rec_play_all);
    releaseVector(&rec_play_next_chunk);
    releaseVector(&rec_play_u8_buffer);
}

bool refillInput()
{
    if (!mp3_fat_open) return false;
    if (mp3_pos > 0 && (mp3_pos > mp3_buf.size() / 2 || mp3_len - mp3_pos < 8192)) {
        const size_t remain = mp3_len - mp3_pos;
        memmove(mp3_buf.data(), mp3_buf.data() + mp3_pos, remain);
        mp3_len = remain;
        mp3_pos = 0;
    }
    while (!mp3_eof && mp3_len < mp3_buf.size()) {
        UINT n = 0;
        const FRESULT result = f_read(&mp3_fat_file, mp3_buf.data() + mp3_len,
                                      mp3_buf.size() - mp3_len, &n);
        if (result != FR_OK) {
            mp3_eof = true;
            break;
        }
        mp3_len += n;
        if (n == 0) mp3_eof = true;
    }
    return mp3_len > mp3_pos;
}

bool isValidMp3FrameHeader(const uint8_t* p)
{
    if (!p || p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return false;
    const int version = (p[1] >> 3) & 0x03;
    const int layer = (p[1] >> 1) & 0x03;
    const int bitrate = (p[2] >> 4) & 0x0F;
    const int sample_rate = (p[2] >> 2) & 0x03;
    return version != 1 && layer == 1 && bitrate != 0 && bitrate != 15 && sample_rate != 3;
}

size_t findSync(size_t start)
{
    for (size_t i = start; i + 3 < mp3_len; ++i) {
        if (isValidMp3FrameHeader(mp3_buf.data() + i)) return i;
    }
    return std::string::npos;
}

bool seekPastId3(std::string* err = nullptr)
{
    uint8_t header[10] = {};
    if (f_lseek(&mp3_fat_file, 0) != FR_OK) {
        if (err) *err = "fat seek failed";
        return false;
    }
    UINT n = 0;
    if (f_read(&mp3_fat_file, header, sizeof(header), &n) != FR_OK) {
        if (err) *err = "fat read failed";
        return false;
    }
    if (n < sizeof(header) || std::memcmp(header, "ID3", 3) != 0) {
        if (f_lseek(&mp3_fat_file, 0) != FR_OK) {
            if (err) *err = "fat seek failed";
            return false;
        }
        return true;
    }
    for (int i = 6; i < 10; ++i) {
        if (header[i] & 0x80) {
            if (err) *err = "bad ID3 size";
            return false;
        }
    }
    const uint32_t tag_size = (static_cast<uint32_t>(header[6]) << 21) |
                              (static_cast<uint32_t>(header[7]) << 14) |
                              (static_cast<uint32_t>(header[8]) << 7) |
                              static_cast<uint32_t>(header[9]);
    const long audio_offset = 10L + static_cast<long>(tag_size) + ((header[5] & 0x10) ? 10L : 0L);
    if (f_size(&mp3_fat_file) <= static_cast<FSIZE_t>(audio_offset) ||
        f_lseek(&mp3_fat_file, static_cast<FSIZE_t>(audio_offset)) != FR_OK) {
        if (err) *err = "bad ID3 offset";
        return false;
    }
    return true;
}

bool decodeChunk(std::string* err);

bool startPlaybackOnce(std::string* err = nullptr)
{
    stopPlayback();
    if (tracks.empty()) {
        if (err) *err = "no tracks";
        return false;
    }
    if (!hasMusicPlaybackHeap(err)) return false;
    std::string path = override_music_path;
    FRESULT open_result = FR_OK;
    if (override_music_path.empty()) {
        open_result = openFatMusicFile(tracks[selected_track], &mp3_fat_file, &path, err);
    } else {
        open_result = f_open(&mp3_fat_file, sdPathToFatPath(path).c_str(), FA_READ);
    }
    if (open_result != FR_OK) {
        if (err && (override_music_path.size() > 0 || err->empty())) {
            if (open_result == FR_INVALID_NAME) {
                *err = "Unsupported filename";
            } else {
                *err = "fat open ";
                *err += std::to_string(static_cast<int>(open_result));
            }
        }
        return false;
    }
    mp3_fat_open = true;
    if (f_size(&mp3_fat_file) < 64) {
        f_close(&mp3_fat_file);
        mp3_fat_open = false;
        if (err) *err = "file too small";
        return false;
    }
    if (!seekPastId3(err)) {
        f_close(&mp3_fat_file);
        mp3_fat_open = false;
        return false;
    }
    mp3dec_init(&mp3_dec);
    mp3_buf.assign(INPUT_BUF_SIZE, 0);
    mp3_frame_pcm.assign(MINIMP3_MAX_SAMPLES_PER_FRAME, 0);
    mp3_len = 0;
    mp3_pos = 0;
    mp3_eof = false;
    pcm_chunk.clear();
    pcm_next_chunk.clear();
    pcm_next_ready = false;
    decoded_chunks = 0;
    music_underruns = 0;
    playing = true;
    if (!decodeChunk(err)) {
        stopPlayback();
        return false;
    }
    M5.Mic.end();
    M5.Speaker.begin();
    applyVolume();
    decoded_chunks = 1;
    M5.Speaker.playRaw(pcm_chunk.data(), pcm_chunk.size(), pcm_rate, false);
    playback_decode_after_ms = M5.millis() + 100;
    screen = Screen::MusicPlaying;
    dirty = true;
    blockInput(400);
    appendInboxEvent("LISTEN", override_music_path.empty() ? tracks[selected_track] : override_music_path.substr(override_music_path.find_last_of('/') + 1));
    return true;
}

bool playbackFailureMayBeSdTransient(const std::string& err)
{
    return err.find("stat failed") != std::string::npos ||
           err.find("open failed") != std::string::npos ||
           err.find("seek failed") != std::string::npos ||
           err.find("no mpeg sync") != std::string::npos ||
           err.find("decode produced no pcm") != std::string::npos;
}

bool startPlayback(std::string* err = nullptr)
{
    std::string first_error;
    if (startPlaybackOnce(&first_error)) return true;

    // Settings writes CONFIG.TXT on the shared FAT volume. Cardputer ADV can
    // leave the next VFS read in a transient failure state; retry once after a
    // controlled reprobe instead of mislabeling every valid track as BAD MP3.
    if (!playbackFailureMayBeSdTransient(first_error) || !manualSdReprobe()) {
        if (err) *err = first_error;
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    std::string retry_error;
    if (startPlaybackOnce(&retry_error)) return true;
    if (err) *err = retry_error.empty() ? first_error : retry_error;
    return false;
}

bool playNextAvailableTrack(int delta, std::string* err = nullptr);

void nextTrack(int delta)
{
    if (tracks.empty()) return;
    advanceTrackSelection(delta);
    if (playing) {
        std::string err;
        const std::string path = selectedPath();
        if (!startPlayback(&err)) {
            if (!shuffle_on || !playNextAvailableTrack(delta, &err)) {
                showMessage("BAD MP3", musicProblemBody(path, err), MessageReturn::Music);
                blockInput(500);
            }
        }
    }
    dirty = true;
}

bool playNextAvailableTrack(int delta, std::string* err)
{
    if (tracks.empty()) {
        if (err) *err = "no tracks";
        return false;
    }
    const int original = selected_track;
    const int original_shuffle_position = shuffle_position;
    for (size_t attempt = 0; attempt < tracks.size(); ++attempt) {
        advanceTrackSelection(delta);
        std::string local_err;
        if (startPlayback(&local_err)) return true;
        if (err && err->empty()) *err = tracks[selected_track] + ": " + local_err;
    }
    selected_track = original;
    shuffle_position = original_shuffle_position;
    return false;
}

void drawWaveform(const std::vector<int16_t>& pcm, int channels)
{
    if (display_off || pcm.empty()) return;
    const int x = 8, y = 76, w = 224, h = 38;
    const uint16_t signal = uiSignal();
    canvas.fillRect(x, y, w, h, uiBg());
    canvas.drawRect(x, y, w, h, uiDim());
    canvas.drawFastHLine(x + 1, y + 1, w - 2, signal);
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
        if (px > 0) canvas.drawLine(prev_x, prev_y, xx, yy, signal);
        prev_x = xx;
        prev_y = yy;
    }
}

int16_t safeAudioSample(int32_t v)
{
    v >>= AUDIO_SAFE_SHIFT;
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return static_cast<int16_t>(v);
}

bool decodeChunkInto(std::vector<int16_t>& out, std::string* err = nullptr)
{
    if (!playing || !mp3_fat_open) {
        if (err) *err = "not playing";
        return false;
    }
    out.clear();
    int target_values = AUDIO_OUTPUT_RATE * CHUNK_MS / 1000;
    if (mp3_frame_pcm.size() < MINIMP3_MAX_SAMPLES_PER_FRAME) {
        mp3_frame_pcm.assign(MINIMP3_MAX_SAMPLES_PER_FRAME, 0);
    }
    out.reserve(target_values + MINIMP3_MAX_SAMPLES_PER_FRAME);
    bool saw_sync = false;
    for (int attempts = 0; attempts < 512 && static_cast<int>(out.size()) < target_values;) {
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
        int samples = mp3dec_decode_frame(&mp3_dec, mp3_buf.data() + mp3_pos, static_cast<int>(mp3_len - mp3_pos), mp3_frame_pcm.data(), &info);
        ++attempts;
        if (samples > 0 && info.frame_bytes > 0 && info.channels > 0) {
            pcm_rate = AUDIO_OUTPUT_RATE;
            pcm_channels = 1;
            const int src_rate = info.hz > 0 ? info.hz : AUDIO_OUTPUT_RATE;
            const int out_samples = std::max(1, samples * AUDIO_OUTPUT_RATE / src_rate);
            out.reserve(out.size() + out_samples);
            for (int out_i = 0; out_i < out_samples; ++out_i) {
                int src_i = out_i * src_rate / AUDIO_OUTPUT_RATE;
                if (src_i >= samples) src_i = samples - 1;
                int32_t sample = 0;
                if (info.channels == 2) {
                    sample = (static_cast<int32_t>(mp3_frame_pcm[src_i * 2]) +
                              static_cast<int32_t>(mp3_frame_pcm[src_i * 2 + 1])) / 2;
                } else {
                    sample = mp3_frame_pcm[src_i * info.channels];
                }
                out.push_back(safeAudioSample(sample));
            }
            mp3_pos += info.frame_bytes;
            target_values = std::max(1024, AUDIO_OUTPUT_RATE * CHUNK_MS / 1000);
        } else {
            ++mp3_pos;
        }
    }
    if (out.empty()) {
        if (err) {
            if (!saw_sync) *err = mp3_eof ? "no mpeg sync / eof" : "no mpeg sync";
            else *err = "decode produced no pcm";
        }
        return false;
    }
    return true;
}

bool decodeChunk(std::string* err = nullptr)
{
    return decodeChunkInto(pcm_chunk, err);
}

void updateAudio()
{
    if (!playing) return;
    if (M5.millis() < playback_decode_after_ms) return;
    std::string err;
    if (M5.Speaker.isPlaying()) {
        if (!pcm_next_ready) {
            if (decodeChunkInto(pcm_next_chunk, &err)) {
                pcm_next_ready = true;
            }
        }
        return;
    }

    if (pcm_next_ready) {
        pcm_chunk.swap(pcm_next_chunk);
        pcm_next_chunk.clear();
        pcm_next_ready = false;
    } else if (!decodeChunk(&err)) {
        const bool eof = mp3_eof && err.find("eof") != std::string::npos;
        if (eof) {
            if (tracks.size() > 1 && override_music_path.empty()) {
                std::string next_err;
                if (!playNextAvailableTrack(1, &next_err)) {
                    stopPlayback();
                    showMessage("Playback stopped", next_err.empty() ? "no playable track" : next_err, MessageReturn::Music);
                }
            } else {
                stopPlayback();
                screen = Screen::MusicList;
            }
        } else {
            const std::string path = override_music_path.empty() ? selectedPath() : override_music_path;
            stopPlayback();
            showMessage("BAD MP3", musicProblemBody(path, err), MessageReturn::Music);
        }
        dirty = true;
        blockInput(350);
        return;
    } else if (decoded_chunks > 0) {
        ++music_underruns;
    }
    ++decoded_chunks;
    if (!display_off) dirty = true;
    M5.Speaker.playRaw(pcm_chunk.data(), pcm_chunk.size(), pcm_rate, false);
}


bool startRecording(std::string* err = nullptr)
{
    releaseTransientBuffersForVoice();
    if (!initVoiceStorage()) {
        if (err) *err = "voice storage";
        return false;
    }
    clearCaptureChunks();
    active_recording_name = newRecordingNameFromMillis();
    rec_samples_written = 0;
    rec_write_error = false;
    rec_write_error_text.clear();
    rec_started_ms = M5.millis();
    rec_auto_stopped = false;
    rec_save_pending = false;
    rec_save_due_ms = 0;
    rec_mic_chunks = 0;
    rec_last_take = 0;
    rec_buffer.assign(REC_BUFFER_SAMPLES, 0);
    // Cardputer ADV has no PSRAM. This voice-note profile reserves ~80 KB
    // before Mic.begin(), then writes only after Mic.end().
    rec_requested_seconds = REC_RECORD_SECONDS;
    rec_target_seconds = REC_RECORD_SECONDS;
    rec_target_ms = REC_RECORD_SECONDS * 1000u;
    rec_record_sample_rate = REC_RECORD_SAMPLE_RATE;
    rec_record_bits = REC_RECORD_BITS;
    rec_capture_capacity = static_cast<size_t>(REC_RECORD_SECONDS) * rec_record_sample_rate;
    rec_capture_byte_capacity = rec_capture_capacity * (rec_record_bits / 8);
    rec_u8_buffer.assign(REC_BUFFER_SAMPLES, 0);
    if (!prepareCaptureChunks()) {
        if (err) {
            const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
            const size_t free_bytes = heap_caps_get_free_size(MALLOC_CAP_8BIT);
            *err = "ram L" + std::to_string(static_cast<unsigned long long>(largest));
            *err += " F" + std::to_string(static_cast<unsigned long long>(free_bytes));
        }
        return false;
    }

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

bool saveCapturedRecording(std::string* err = nullptr)
{
    if (rec_samples_written == 0 || rec_capture_chunks.empty()) {
        if (err) *err = "empty";
        return false;
    }
    if (!initVoiceStorage()) {
        if (err) *err = "voice storage";
        return false;
    }
    recordings_dir = VOICE_STORE_DIR;
    FILE* f = nullptr;
    std::string path;
    int last_errno = ENOENT;
    for (int i = 0; i < 1000; ++i) {
        if (i > 0) {
            char buf[16];
            snprintf(buf, sizeof(buf), "REC%05lu.WAV", static_cast<unsigned long>(((M5.millis() / 100) + i) % 100000));
            active_recording_name = buf;
        }
        path = recordings_dir + "/" + active_recording_name;
        struct stat st = {};
        if (stat(path.c_str(), &st) == 0) continue;
        f = fopen(path.c_str(), "wb");
        if (f) break;
        last_errno = errno;
    }
    if (!f) {
        if (err) *err = "open: " + std::to_string(last_errno);
        return false;
    }
    uint8_t header[44] = {};
    buildWavHeader(header, rec_samples_written, rec_record_sample_rate, rec_record_bits);
    bool ok = true;
    const char* failure_stage = "";
    int failure_errno = 0;
    if (fwrite(header, 1, sizeof(header), f) != sizeof(header)) {
        ok = false;
        failure_stage = "header";
        failure_errno = errno;
    }
    size_t bytes_left = rec_samples_written * (rec_record_bits / 8);
    for (const auto& chunk : rec_capture_chunks) {
        if (!chunk.data || chunk.used == 0) continue;
        const size_t todo = std::min<size_t>(bytes_left, chunk.used);
        size_t wrote_total = 0;
        const uint8_t* base = chunk.data;
        while (ok && wrote_total < todo) {
            const size_t todo_now = std::min<size_t>(REC_SAVE_CHUNK_SAMPLES * sizeof(int16_t), todo - wrote_total);
            const size_t wrote = fwrite(base + wrote_total, 1, todo_now, f);
            wrote_total += wrote;
            if (wrote != todo_now) {
                ok = false;
                failure_stage = "write";
                failure_errno = errno;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        bytes_left -= todo;
        if (!ok) break;
        if (bytes_left == 0) break;
    }
    if (bytes_left != 0 && ok) {
        ok = false;
        failure_stage = "short";
    }
    if (fflush(f) != 0) {
        ok = false;
        failure_stage = "flush";
        failure_errno = errno;
    }
    if (fclose(f) != 0) {
        ok = false;
        failure_stage = "close";
        failure_errno = errno;
    }
    if (!ok) {
        if (err) *err = std::string(failure_stage[0] ? failure_stage : "save") + ": " + std::to_string(failure_errno);
        std::remove(path.c_str());
        return false;
    }
    return true;
}

void finishRecordingStop(bool failed, const std::string& failed_text)
{
    M5.Speaker.begin();
    applyVolume();
    if (failed) {
        showMessage("Record failed", failed_text, MessageReturn::Recorder);
    } else {
        if (std::find(recordings.begin(), recordings.end(), active_recording_name) == recordings.end()) {
            recordings.push_back(active_recording_name);
            std::sort(recordings.begin(), recordings.end());
        }
        auto it = std::find(recordings.begin(), recordings.end(), active_recording_name);
        if (it != recordings.end()) {
            recorder_cursor = static_cast<int>(std::distance(recordings.begin(), it)) + 1;
        }
        char rec_dur[12];
        const uint32_t rec_ms = recSecondsMsFromSamples(rec_samples_written);
        const int32_t delta_ms = recClockDeltaMs();
        formatTenthsSeconds(rec_ms, rec_dur, sizeof(rec_dur));
        char stop_line[24] = "";
        if (delta_ms >= 0) {
            snprintf(stop_line, sizeof(stop_line), "%s +%ldms", rec_auto_stopped ? "AUTO" : "MAN", static_cast<long>(delta_ms));
        } else {
            snprintf(stop_line, sizeof(stop_line), "%s %ldms", rec_auto_stopped ? "AUTO" : "MAN", static_cast<long>(delta_ms));
        }
        std::string dur_line = std::string(rec_dur) + " sec\n" + std::string(stop_line);
        // RAM queue only. The main loop persists this to internal SPIFFS after
        // Voice exits; no SD or flash write occurs in the audio completion path.
        appendInboxEvent("VOICE", active_recording_name);
        showMessage("Record saved", active_recording_name + "\n" + dur_line, MessageReturn::Recorder);
    }
    rec_write_error = false;
    rec_write_error_text.clear();
    rec_mic_chunks = 0;
    rec_last_take = 0;
    rec_auto_stopped = false;
    rec_save_pending = false;
    rec_save_due_ms = 0;
    rec_stopped_ms = 0;
    clearCaptureChunks();
    rec_capture_capacity = 0;
    rec_target_ms = REC_RECORD_SECONDS * 1000u;
    rec_record_sample_rate = REC_RECORD_SAMPLE_RATE;
    rec_record_bits = REC_RECORD_BITS;
    dirty = true;
    blockInput(400);
}

void stopRecording(bool save)
{
    if (rec_save_pending) return;
    rec_stopped_ms = M5.millis();
    const uint32_t mic_stop_deadline = M5.millis() + 300;
    while (M5.Mic.isRecording() && static_cast<int32_t>(M5.millis() - mic_stop_deadline) < 0) {
        M5.update();
        M5.delay(1);
    }
    M5.Mic.end();
    bool failed = rec_write_error;
    std::string failed_text = rec_write_error_text.empty() ? "write failed" : rec_write_error_text;
    if (save && !failed) {
        rec_save_pending = true;
        rec_save_due_ms = M5.millis() + 500;
        dirty = true;
        return;
    }
    finishRecordingStop(failed, failed_text);
}

void updateRecording()
{
    if (screen != Screen::RecorderRecording || rec_buffer.empty()) return;
    if (rec_save_pending) {
        if (static_cast<int32_t>(M5.millis() - rec_save_due_ms) < 0) {
            dirty = true;
            return;
        }
        rec_save_pending = false;
        std::string err;
        const bool ok = saveCapturedRecording(&err);
        finishRecordingStop(!ok, ok ? "" : (err.empty() ? "save failed" : err));
        return;
    }
    if (rec_write_error) return;
    if (M5.Mic.record(rec_buffer.data(), rec_buffer.size(), rec_record_sample_rate)) {
        const size_t room = rec_capture_capacity > rec_samples_written ? rec_capture_capacity - rec_samples_written : 0;

        if (room == 0) {
            if (rec_capture_capacity > 0 || recTimeReached()) {
                rec_auto_stopped = true;
                stopRecording(true);
            }
            return;
        }

        size_t take = std::min(room, rec_buffer.size());

        bool captured = false;
        if (rec_record_bits == 8) {
            rec_u8_buffer.resize(take);
            for (size_t i = 0; i < take; ++i) {
                const int32_t unsigned_sample = (static_cast<int32_t>(rec_buffer[i]) + 32768) >> 8;
                rec_u8_buffer[i] = static_cast<uint8_t>(std::max<int32_t>(0, std::min<int32_t>(255, unsigned_sample)));
            }
            captured = appendCaptureChunk(rec_u8_buffer.data(), take);
        } else {
            captured = appendCaptureChunk(rec_buffer.data(), take * sizeof(int16_t));
        }
        if (!captured) {
            rec_write_error = true;
            rec_write_error_text = "ram alloc";
            stopRecording(true);
            return;
        }
        rec_samples_written += static_cast<uint32_t>(take);
        ++rec_mic_chunks;
        rec_last_take = take;
        pcm_chunk = rec_buffer;
        pcm_channels = 1;
        pcm_rate = rec_record_sample_rate;
        if (!display_off) dirty = true;

        if (rec_capture_capacity > 0 && rec_samples_written >= rec_capture_capacity) {
            rec_auto_stopped = true;
            stopRecording(true);
        } else if (recTimeReached()) {
            rec_auto_stopped = true;
            stopRecording(true);
        }
    }
}

std::string selectedRecordingPath()
{
    if (recordings.empty() || recorder_cursor <= 0) return "";
    return recordings_dir + "/" + recordings[recorder_cursor - 1];
}

std::string recordingPathByName(const std::string& name)
{
    return recordings_dir + "/" + name;
}

std::string recordingMetaLine(const std::string& name)
{
    const std::string path = recordingPathByName(name);
    struct stat st = {};
    if (stat(path.c_str(), &st) != 0) return "missing";
    const std::string ext = lowerExt(name);
    uint32_t seconds = 0;
    if (ext == ".wav" && st.st_size > 44) {
        FILE* f = fopen(path.c_str(), "rb");
        uint32_t offset = 44;
        uint32_t rate = REC_SAMPLE_RATE;
        uint16_t bits = 16;
        if (f && readWavHeader(f, &offset, &rate, &bits, nullptr) && rate > 0) {
            seconds = static_cast<uint32_t>((st.st_size - offset) / (bits / 8) / rate);
        }
        if (f) fclose(f);
    } else if (ext == ".pcm") {
        seconds = static_cast<uint32_t>(st.st_size / sizeof(int16_t) / REC_SAMPLE_RATE);
    }
    char buf[48];
    snprintf(buf, sizeof(buf), "%s  %lus  %luB",
             ext == ".pcm" ? "PCM" : "WAV",
             static_cast<unsigned long>(seconds),
             static_cast<unsigned long>(st.st_size));
    return buf;
}

bool startRecordingPlayback(std::string* err = nullptr)
{
    stopPlayback();
    rec_play_all.clear();
    if (rec_play_file) {
        fclose(rec_play_file);
        rec_play_file = nullptr;
    }
    if (recorder_cursor <= 0 || recordings.empty()) {
        if (err) *err = "no file";
        return false;
    }
    const std::string path = selectedRecordingPath();
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        if (err) {
            *err = "open: ";
            *err += std::strerror(errno);
        }
        return false;
    }
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    const bool wav = lowerExt(path) == ".wav";
    uint32_t wav_offset = 0;
    uint32_t wav_rate = REC_SAMPLE_RATE;
    uint16_t wav_bits = 16;
    if (wav && !readWavHeader(f, &wav_offset, &wav_rate, &wav_bits, err)) {
        fclose(f);
        return false;
    }
    const long data_offset = wav ? static_cast<long>(wav_offset) : 0;
    if (file_size <= data_offset) {
        fclose(f);
        if (err) *err = "empty";
        return false;
    }
    const size_t bytes_per_sample = wav ? wav_bits / 8 : sizeof(int16_t);
    const size_t samples = static_cast<size_t>((file_size - data_offset) / bytes_per_sample);
    if (samples < (wav ? wav_rate : REC_SAMPLE_RATE) / 4) {
        fclose(f);
        if (err) *err = "bad rec";
        return false;
    }
    fseek(f, data_offset, SEEK_SET);
    rec_play_file = f;
    rec_play_chunks = 0;
    rec_play_next_ready = false;
    rec_play_next_chunk.clear();
    rec_play_next_last = false;
    rec_play_current_last = false;
    active_recording_name = recordings[recorder_cursor - 1];
    M5.Mic.end();
    M5.Speaker.begin();
    applyVolume();
    pcm_chunk.clear();
    pcm_channels = 1;
    pcm_rate = wav ? static_cast<int>(wav_rate) : REC_SAMPLE_RATE;
    rec_play_bits = wav ? wav_bits : 16;
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
    rec_play_all.clear();
    rec_play_next_chunk.clear();
    rec_play_next_ready = false;
    rec_play_next_last = false;
    rec_play_current_last = false;
    rec_play_bits = 16;
    rec_play_u8_buffer.clear();
    screen = Screen::RecorderList;
    dirty = true;
    blockInput(350);
}

bool readNextRecPlayChunk(std::vector<int16_t>& out, bool* out_partial = nullptr)
{
    if (!rec_play_file) return false;
    constexpr size_t output_samples = REC_BUFFER_SAMPLES * 4;
    if (rec_play_bits == 8) {
        rec_play_u8_buffer.resize(output_samples);
        const size_t n = fread(rec_play_u8_buffer.data(), 1, rec_play_u8_buffer.size(), rec_play_file);
        if (n == 0) return false;
        out.resize(n);
        for (size_t i = 0; i < n; ++i) {
            out[i] = static_cast<int16_t>((static_cast<int>(rec_play_u8_buffer[i]) - 128) << 8);
        }
        if (out_partial) *out_partial = (n < output_samples);
        return true;
    }
    if (out.size() != output_samples) out.assign(output_samples, 0);
    const size_t n = fread(out.data(), sizeof(int16_t), out.size(), rec_play_file);
    if (n == 0) return false;
    if (out_partial) *out_partial = (n < output_samples);
    out.resize(n);
    return true;
}

void updateRecordingPlayback()
{
    if (screen != Screen::RecorderPlaying) return;
    if (M5.Speaker.isPlaying()) {
        if (!rec_play_next_ready) {
            bool partial = false;
            const bool got_next = readNextRecPlayChunk(rec_play_next_chunk, &partial);
            rec_play_next_ready = got_next;
            rec_play_next_last = partial;
            if (!got_next) {
                rec_play_current_last = true;
            }
        }
        return;
    }
    if (!rec_play_file) {
        stopRecordingPlayback();
        return;
    }
    if (rec_play_current_last) {
        stopRecordingPlayback();
        return;
    }
    if (rec_play_next_ready) {
        pcm_chunk.swap(rec_play_next_chunk);
        rec_play_next_ready = false;
        rec_play_next_chunk.clear();
        rec_play_current_last = rec_play_next_last;
    }
    if (pcm_chunk.empty()) {
        bool partial = false;
        if (!readNextRecPlayChunk(pcm_chunk, &partial)) {
            stopRecordingPlayback();
            return;
        }
        rec_play_current_last = partial;
    }
    if (pcm_chunk.empty()) {
        stopRecordingPlayback();
        return;
    }
    ++rec_play_chunks;
    if (!display_off) dirty = true;
    M5.Speaker.playRaw(pcm_chunk.data(), pcm_chunk.size(), pcm_rate, false);
}


int batteryPercent()
{
    static int stable_level = -1;
    static int pending_drop = -1;
    static uint32_t last_sample_ms = 0;
    const uint32_t now = M5.millis();
    if (last_sample_ms != 0 && now - last_sample_ms < 2000) return stable_level;
    last_sample_ms = now;

    int level = -1;
    // The first ADC conversion after a display/app transition can be zero on
    // Cardputer ADV. Take a few short samples before declaring it unavailable.
    for (int attempt = 0; attempt < 3; ++attempt) {
        level = M5.Power.getBatteryLevel();
        if (level > 0 && level <= 100) break;
        if (attempt < 2) vTaskDelay(pdMS_TO_TICKS(12));
    }
    battery_last_level = level;
    battery_last_mv = M5.Power.getBatteryVoltage();
    // The voltage path is independent at the public API boundary. Use it if
    // level reporting is unavailable but the Cardputer ADC returned a real
    // Li-Po voltage.
    if ((level <= 0 || level > 100) && battery_last_mv >= 3300 && battery_last_mv <= 4300) {
        level = std::max(0, std::min(100, (battery_last_mv - 3300) * 100 / 800));
    }
    if (level < 0 || level > 100) return stable_level;
    // Cardputer ADV occasionally reports one false 0% while running from the
    // battery. Until there is a credible sample, show --% rather than a false
    // flat-battery warning; afterwards keep the last credible value.
    if (level == 0 && !(battery_last_mv >= 2800 && battery_last_mv <= 4300)) {
        if (stable_level < 0 || stable_level > 5) return stable_level;
    }
    if (stable_level < 0 || level >= stable_level || level >= stable_level - 12) {
        stable_level = level;
        pending_drop = -1;
    } else if (pending_drop == level) {
        stable_level = level;
        pending_drop = -1;
    } else {
        pending_drop = level;
    }
    return stable_level;
}

void drawBatteryWidget(int x, int y)
{
    int level = batteryPercent();
    canvas.drawLine(x + 4, y, x, y + 7, uiFg());
    canvas.drawLine(x, y + 7, x + 5, y + 7, uiFg());
    canvas.drawLine(x + 5, y + 7, x + 2, y + 13, uiFg());
    canvas.setTextSize(2);
    canvas.setTextColor(uiFg(), uiBg());
    canvas.setCursor(x + 11, y);
    if (level >= 0) canvas.printf("%d%%", level);
    else canvas.print("--%");
}

std::string formatBytes(uint64_t bytes)
{
    char buf[16];
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        snprintf(buf, sizeof(buf), "%.1fG", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ULL * 1024ULL) {
        snprintf(buf, sizeof(buf), "%.0fM", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024ULL) {
        snprintf(buf, sizeof(buf), "%.0fK", static_cast<double>(bytes) / 1024.0);
    } else {
        snprintf(buf, sizeof(buf), "%luB", static_cast<unsigned long>(bytes));
    }
    return buf;
}

bool sdUsage(uint64_t* total, uint64_t* free_bytes)
{
    if (!initSd()) return false;
    FATFS* fs = nullptr;
    DWORD free_clusters = 0;
    FRESULT res = f_getfree("0:", &free_clusters, &fs);
    if (res != FR_OK || fs == nullptr) {
        res = f_getfree("", &free_clusters, &fs);
    }
    if (res != FR_OK || fs == nullptr) return false;
    uint64_t sector_size = 512;
#if FF_MAX_SS != FF_MIN_SS
    sector_size = fs->ssize;
#endif
    *total = static_cast<uint64_t>(fs->n_fatent - 2) * fs->csize * sector_size;
    *free_bytes = static_cast<uint64_t>(free_clusters) * fs->csize * sector_size;
    return true;
}

void openLauncherApp(int index)
{
    if (index >= 0 && index <= 9) last_resume_target = static_cast<ResumeTarget>(index);
    if (index == 0) { scanMusic(); screen = Screen::MusicList; blockInput(250); }
    else if (index == 1) { scanBooks(); screen = Screen::ReaderList; blockInput(250); }
    else if (index == 2) { scanNotes(); screen = Screen::NotesList; blockInput(250); }
    else if (index == 3) { scanRecordings(); screen = Screen::RecorderList; blockInput(250); }
    else if (index == 4) { time_mode = TimeMode::Clock; clock_base_ms = M5.millis(); screen = Screen::TimeApp; blockInput(250); }
    else if (index == 5) { scanFiles(MOUNT_POINT); screen = Screen::FilesList; blockInput(250); }
    else if (index == 6) { random_result = "READY"; screen = Screen::Randomizer; blockInput(250); }
    else if (index == 7) { scanHabits(); screen = Screen::HabitsList; blockInput(250); }
    else if (index == 8) { screen = Screen::Settings; blockInput(250); }
    else if (index == 9) { screen = Screen::Connections; blockInput(250); }
    else if (index == 10) {
        screen = Screen::InboxList;
        refreshInboxManual();
        blockInput(250);
    }
}

const char* resumeName()
{
    switch (last_resume_target) {
        case ResumeTarget::Music: return "LISTEN";
        case ResumeTarget::Reader: return "READ";
        case ResumeTarget::Notes: return "WRITE";
        case ResumeTarget::Recorder: return "VOICE";
        case ResumeTarget::Time: return "TIME";
        case ResumeTarget::Files: return "FILES";
        case ResumeTarget::Randomizer: return "DECIDE";
        case ResumeTarget::Habits: return "ROUTINES";
        case ResumeTarget::Settings: return "SETTINGS";
        case ResumeTarget::Connections: return "TRANSFER";
    }
    return "LISTEN";
}

std::string dashboardClip(std::string text, size_t width)
{
    if (text.empty()) return "-";
    if (text.size() <= width) return text;
    return text.substr(0, width);
}

std::string dashboardMusicName()
{
    if (tracks.empty()) return "-";
    return dashboardClip(musicDisplayName(tracks[std::max(0, std::min(selected_track, static_cast<int>(tracks.size()) - 1))]), 15);
}

std::string dashboardBookName()
{
    return dashboardClip(last_reader_book.empty() ? active_book_name : last_reader_book, 15);
}

std::string dashboardNoteName()
{
    return dashboardClip(last_note_name.empty() ? active_note_name : last_note_name, 15);
}

std::string dashboardRecordingName()
{
    return dashboardClip(active_recording_name, 15);
}

void resumeContext()
{
    switch (last_resume_target) {
        case ResumeTarget::Music:
            scanMusic();
            screen = Screen::MusicList;
            break;
        case ResumeTarget::Reader:
            scanBooks();
            if (reader_opened_this_session && !last_reader_book.empty() && !books.empty()) {
                auto it = std::find(books.begin(), books.end(), last_reader_book);
                if (it != books.end()) selected_book = static_cast<int>(std::distance(books.begin(), it));
                std::string err;
                if (loadSelectedBook(&err)) {
                    screen = Screen::ReaderView;
                    break;
                }
            }
            screen = Screen::ReaderList;
            break;
        case ResumeTarget::Notes:
            scanNotes();
            screen = Screen::NotesList;
            break;
        case ResumeTarget::Recorder:
            scanRecordings();
            screen = Screen::RecorderList;
            break;
        case ResumeTarget::Time:
            time_mode = TimeMode::Clock;
            clock_base_ms = M5.millis();
            screen = Screen::TimeApp;
            break;
        case ResumeTarget::Files:
            scanFiles(files_path.empty() ? std::string(MOUNT_POINT) : files_path);
            screen = Screen::FilesList;
            break;
        case ResumeTarget::Randomizer:
            random_result = "READY";
            screen = Screen::Randomizer;
            break;
        case ResumeTarget::Habits:
            scanHabits();
            screen = Screen::HabitsList;
            break;
        case ResumeTarget::Settings:
            screen = Screen::Settings;
            break;
        case ResumeTarget::Connections:
            screen = Screen::Connections;
            break;
    }
    blockInput(250);
    dirty = true;
}

void openDashboard()
{
    // Read-only refresh for today's routine status. This does not create a
    // new storage format or alter the current Music/Record state.
    scanHabits();
    screen = Screen::Dashboard;
    blockInput(250);
    dirty = true;
}

void pulseUi()
{
    // Visual glitch effects are disabled for now. They were visually ambiguous
    // on hardware and looked like display/input bugs during text work.
    ui_anim_until_ms = 0;
    ui_anim_last_frame_ms = 0;
}

void drawCyberAccent()
{
    // Intentionally empty. Keep call sites so a future stable visual system can
    // be reintroduced centrally without touching every screen.
}

void drawLauncher()
{
    static const char* labels[] = {"[#] LISTEN", "[=] READ", "[+] WRITE", "[o] VOICE", "[~] TIME", "[*] FILES", "[?] DECIDE", "[x] ROUTINES", "[%] SETTINGS", "[~] TRANSFER", "[>] INBOX"};
    constexpr int launcher_count = sizeof(labels) / sizeof(labels[0]);
    canvas.fillScreen(uiBg());
    drawCyberAccent();
    canvas.setTextSize(2);
    canvas.setTextColor(uiFg(), uiBg());
    canvas.setCursor(8, 8);
    canvas.println("ABVx OS");
    canvas.setTextSize(1);
    canvas.setTextColor(uiDim(), uiBg());
    canvas.setCursor(74, 12);
    canvas.printf("%d/%d", launcher_index + 1, launcher_count);
    drawBatteryWidget(166, 8);
    canvas.setTextSize(2);
    int start = std::max(0, launcher_index - 1);
    start = std::min(start, std::max(0, launcher_count - 3));
    for (int i = start; i < std::min(launcher_count, start + 3); ++i) {
        canvas.setCursor(8, 38 + (i - start) * 24);
        canvas.setTextColor(i == launcher_index ? uiBg() : uiFg(), i == launcher_index ? uiFg() : uiBg());
        canvas.printf("%c %s", i == launcher_index ? '>' : ' ', labels[i]);
    }
    canvas.setTextSize(1);
    canvas.setTextColor(uiDim(), uiBg());
    canvas.setCursor(8, 106);
    canvas.printf("2/S RESUME %s", resumeName());
    canvas.setCursor(8, 122);
    canvas.print("OK OPEN  D DASH  R/N/M");
    canvas.pushSprite(0, 0);
}

void drawDashboard()
{
    canvas.fillScreen(uiBg());
    drawCyberAccent();
    canvas.setTextSize(2);
    canvas.setTextColor(uiFg(), uiBg());
    canvas.setCursor(8, 8);
    canvas.print("DASH");
    drawBatteryWidget(166, 8);

    char clock_text[16];
    formatHMS(elapsedClockSeconds(), clock_text, sizeof(clock_text));
    canvas.setTextSize(3);
    canvas.setTextColor(uiAccent(), uiBg());
    canvas.setCursor(42, 30);
    canvas.print(clock_text);

    int done_count = 0;
    for (const auto& h : habits) if (h.done) ++done_count;
    const int habit_total = static_cast<int>(habits.size());
    const int habit_pct = habit_total > 0 ? (done_count * 100) / habit_total : 0;

    canvas.setTextSize(1);
    canvas.setTextColor(uiAccent(), uiBg());
    canvas.setCursor(8, 64);
    canvas.printf("RESUME %-8s OK", resumeName());
    canvas.setCursor(8, 76);
    canvas.printf("TODAY %d/%d  %d%%", done_count, habit_total, habit_pct);

    canvas.setCursor(8, 88);
    canvas.printf("M %.15s", dashboardMusicName().c_str());

    canvas.setTextSize(1);
    canvas.setTextColor(uiDim(), uiBg());
    canvas.setCursor(8, 102);
    canvas.printf("B %.15s", dashboardBookName().c_str());
    canvas.setCursor(124, 102);
    canvas.printf("N %.10s", dashboardNoteName().c_str());
    canvas.setCursor(8, 114);
    canvas.printf("V %.15s", dashboardRecordingName().c_str());
    canvas.setCursor(8, 124);
    canvas.print("1 LISTEN 2 READ 3 WRITE GO BACK");
    canvas.pushSprite(0, 0);
}

void drawRandomizer()
{
    canvas.fillScreen(uiBg());
    drawCyberAccent();
    canvas.setTextSize(2);
    canvas.setTextColor(uiFg(), uiBg());
    canvas.setCursor(8, 8);
    canvas.print("DECIDE");
    canvas.setTextSize(4);
    canvas.setTextColor(uiAccent(), uiBg());
    int result_x = std::max(0, (SCREEN_W - static_cast<int>(canvas.textWidth(random_result.c_str()))) / 2);
    canvas.setCursor(result_x, 50);
    canvas.print(random_result.c_str());
    canvas.setTextSize(1);
    canvas.setTextColor(uiDim(), uiBg());
    canvas.setCursor(8, 98);
    canvas.print("HIST ");
    for (size_t i = 0; i < random_history.size(); ++i) {
        if (i) canvas.print(" ");
        canvas.print(random_history[i].c_str());
    }
    canvas.setTextSize(1);
    canvas.setTextColor(uiDim(), uiBg());
    canvas.setCursor(8, 122);
    canvas.print("OK ROLL   GO BACK");
    canvas.pushSprite(0, 0);
}

void drawInboxList()
{
    canvas.fillScreen(uiBg());
    drawCyberAccent();
    canvas.setTextSize(2);
    canvas.setTextColor(uiFg(), uiBg());
    canvas.setCursor(8, 8);
    canvas.print("INBOX");
    canvas.setTextSize(1);
    canvas.setTextColor(uiAccent(), uiBg());
    canvas.setCursor(130, 14);
    canvas.printf("%d P%d", static_cast<int>(inbox_entries.size()), static_cast<int>(inbox_pending_events.size()));
    canvas.setTextColor(uiDim(), uiBg());
    canvas.setCursor(8, 28);
    canvas.printf("store %s", inbox_status.c_str());
    if (inbox_entries.empty()) {
        canvas.setTextSize(2);
        canvas.setTextColor(uiFg(), uiBg());
        canvas.setCursor(8, 54);
        canvas.print("NO EVENTS");
        canvas.setTextSize(1);
        canvas.setTextColor(uiDim(), uiBg());
        canvas.setCursor(8, 84);
        canvas.print("open/save/play to log");
    } else {
        const int rows = 3;
        int start = std::max(0, inbox_cursor - 1);
        start = std::min(start, std::max(0, static_cast<int>(inbox_entries.size()) - rows));
        const int end = std::min(static_cast<int>(inbox_entries.size()), start + rows);
        canvas.setTextSize(2);
        for (int i = start; i < end; ++i) {
            InboxEventView ev = parseInboxEvent(inbox_entries[i]);
            canvas.setCursor(8, 42 + (i - start) * 26);
            canvas.setTextColor(i == inbox_cursor ? uiBg() : uiFg(), i == inbox_cursor ? uiFg() : uiBg());
            canvas.printf("%c%.6s %.10s", i == inbox_cursor ? '>' : ' ', ev.type.c_str(), ev.detail.c_str());
        }
    }
    canvas.setTextSize(1);
    canvas.setTextColor(uiDim(), uiBg());
    canvas.setCursor(8, 122);
    canvas.print("OK DETAIL  1 REFRESH  GO BACK");
    canvas.pushSprite(0, 0);
}

void drawInboxDetail()
{
    canvas.fillScreen(uiBg());
    drawCyberAccent();
    canvas.setTextSize(2);
    canvas.setTextColor(uiFg(), uiBg());
    canvas.setCursor(8, 8);
    canvas.print("EVENT");
    canvas.setTextSize(1);
    canvas.setTextColor(uiDim(), uiBg());
    canvas.setCursor(124, 14);
    canvas.printf("%d/%d", inbox_entries.empty() ? 0 : inbox_cursor + 1, static_cast<int>(inbox_entries.size()));
    if (inbox_entries.empty()) {
        canvas.setTextSize(2);
        canvas.setTextColor(uiFg(), uiBg());
        canvas.setCursor(8, 54);
        canvas.print("NO EVENT");
    } else {
        InboxEventView ev = parseInboxEvent(inbox_entries[inbox_cursor]);
        canvas.setTextSize(2);
        canvas.setTextColor(uiAccent(), uiBg());
        canvas.setCursor(8, 38);
        canvas.printf("%.10s", ev.type.c_str());
        canvas.setTextSize(1);
        canvas.setTextColor(uiDim(), uiBg());
        canvas.setCursor(8, 64);
        canvas.printf("time %s", ev.stamp.empty() ? "-" : ev.stamp.c_str());
        canvas.setTextColor(uiFg(), uiBg());
        canvas.setCursor(8, 84);
        canvas.printf("%.32s", ev.detail.c_str());
        if (ev.detail.size() > 32) {
            canvas.setCursor(8, 98);
            canvas.printf("%.32s", ev.detail.c_str() + 32);
        }
    }
    canvas.setTextSize(1);
    canvas.setTextColor(uiDim(), uiBg());
    canvas.setCursor(8, 122);
    canvas.print("OK OPEN   1 REFRESH   GO");
    canvas.pushSprite(0, 0);
}

void drawHabitsList()
{
    canvas.fillScreen(uiBg());
    drawCyberAccent();
    canvas.setTextSize(2);
    canvas.setTextColor(uiFg(), uiBg());
    canvas.setCursor(8, 8);
    canvas.print("ROUTINES");
    int done_count = 0;
    for (const auto& h : habits) if (h.done) ++done_count;
    int pct = habits.empty() ? 0 : (done_count * 100) / static_cast<int>(habits.size());
    canvas.setTextSize(1);
    canvas.setTextColor(uiAccent(), uiBg());
    canvas.setCursor(8, 28);
    canvas.printf("DAY %d  %d/%d  %d%%", habit_day, done_count, static_cast<int>(habits.size()), pct);
    if (habits.empty()) {
        canvas.setTextSize(2);
        canvas.setTextColor(uiFg(), uiBg());
        canvas.setCursor(8, 48);
        canvas.print(disabled_habit_count > 0 ? "NO ACTIVE" : "NO HABITS");
        canvas.setTextSize(1);
        canvas.setTextColor(uiDim(), uiBg());
        canvas.setCursor(8, 78);
        canvas.print(disabled_habit_count > 0 ? "OK RESTORE ALL" : "/sdcard/habits");
    } else {
        int rows = 3;
        int start = std::max(0, habits_cursor - 1);
        start = std::min(start, std::max(0, static_cast<int>(habits.size()) - rows));
        int end = std::min(static_cast<int>(habits.size()), start + rows);
        canvas.setTextSize(2);
        for (int i = start; i < end; ++i) {
            const auto& h = habits[i];
            canvas.setCursor(8, 38 + (i - start) * 24);
            canvas.setTextColor(i == habits_cursor ? uiBg() : uiFg(), i == habits_cursor ? uiFg() : uiBg());
            canvas.printf("%c[%c] %.11s", i == habits_cursor ? '>' : ' ', h.done ? 'x' : ' ', h.title.c_str());
        }
    }
    canvas.setTextSize(1);
    canvas.setTextColor(uiDim(), uiBg());
    canvas.setCursor(8, 112);
    canvas.print("< MANAGE  > STATS  1 NEXT DAY");
    canvas.setCursor(8, 122);
    canvas.print("OK CHECK        GO BACK");
    canvas.pushSprite(0, 0);
}

void drawHabitsStats()
{
    canvas.fillScreen(uiBg());
    drawCyberAccent();
    canvas.setTextSize(2);
    canvas.setTextColor(uiFg(), uiBg());
    canvas.setCursor(8, 8);
    canvas.printf("STATS %dD", habit_stats_window);
    int days = std::max(1, std::min(habit_stats_window, habit_day));
    int start_day = std::max(1, habit_day - days + 1);
    int total_done = 0;
    int total_possible = std::max(1, days * static_cast<int>(habits.size()));
    int rows = std::min(3, static_cast<int>(habits.size()));
    canvas.setTextSize(1);
    canvas.setTextColor(uiAccent(), uiBg());
    canvas.setCursor(8, 28);
    canvas.printf("WINDOW < %dD >", days);
    canvas.setTextSize(2);
    for (int i = 0; i < rows; ++i) {
        int done = habitDoneCount(habits[i].id, start_day, habit_day);
        int streak = habitStreak(habits[i].id);
        total_done += done;
        canvas.setCursor(8, 44 + i * 22);
        canvas.setTextColor(uiFg(), uiBg());
        canvas.printf("%.7s %d/%d S%d", habits[i].title.c_str(), done, days, streak);
    }
    for (int i = rows; i < static_cast<int>(habits.size()); ++i) {
        total_done += habitDoneCount(habits[i].id, start_day, habit_day);
    }
    canvas.setTextSize(1);
    canvas.setTextColor(uiDim(), uiBg());
    canvas.setCursor(8, 106);
    canvas.printf("TOTAL %d/%d %d%%", total_done, total_possible, (total_done * 100) / total_possible);
    canvas.setCursor(8, 122);
    canvas.print("< / >  7D/30D/365D  GO BACK");
    canvas.pushSprite(0, 0);
}

void drawHabitsManage()
{
    static const char* items[] = {"ADD HABIT", "RENAME SEL", "DISABLE SEL", "BACK"};
    canvas.fillScreen(uiBg());
    drawCyberAccent();
    canvas.setTextSize(2);
    canvas.setTextColor(uiFg(), uiBg());
    canvas.setCursor(8, 8);
    canvas.print("MANAGE");
    for (int i = 0; i < 4; ++i) {
        canvas.setCursor(8, 32 + i * 20);
        canvas.setTextColor(i == habits_manage_cursor ? uiBg() : uiFg(), i == habits_manage_cursor ? uiFg() : uiBg());
        canvas.printf("%c %s", i == habits_manage_cursor ? '>' : ' ', items[i]);
    }
    canvas.setTextSize(1);
    canvas.setTextColor(uiDim(), uiBg());
    canvas.setCursor(8, 112);
    if (!habits.empty()) canvas.printf("SEL %.14s", habits[habits_cursor].title.c_str());
    else canvas.print("NO HABITS");
    canvas.setCursor(8, 122);
    canvas.print("OK SELECT       GO BACK");
    canvas.pushSprite(0, 0);
}

void drawHabitsDisableConfirm()
{
    canvas.fillScreen(uiBg());
    canvas.setTextSize(2);
    canvas.setTextColor(uiFg(), uiBg());
    canvas.setCursor(8, 10);
    canvas.print("DISABLE?");
    canvas.setTextSize(1);
    canvas.setTextColor(uiAccent(), uiBg());
    canvas.setCursor(8, 48);
    canvas.printf("%.28s", pending_habit_name.c_str());
    canvas.setTextColor(uiDim(), uiBg());
    canvas.setCursor(8, 104);
    canvas.print("history kept on SD");
    canvas.setCursor(8, 122);
    canvas.print("OK DISABLE       GO CANCEL");
    canvas.pushSprite(0, 0);
}

void drawHabitsEdit()
{
    canvas.fillScreen(uiBg());
    drawCyberAccent();
    canvas.setTextSize(2);
    canvas.setTextColor(uiFg(), uiBg());
    canvas.setCursor(8, 8);
    canvas.print(habit_edit_renaming ? "RENAME" : "ADD HABIT");
    canvas.setTextSize(2);
    canvas.setCursor(8, 48);
    std::string tail = habit_input.size() > 19 ? habit_input.substr(habit_input.size() - 19) : habit_input;
    canvas.print(tail.c_str());
    canvas.print("_");
    canvas.setTextSize(1);
    canvas.setTextColor(uiDim(), uiBg());
    canvas.setCursor(8, 112);
    canvas.printf("%d/32", static_cast<int>(habit_input.size()));
    canvas.setCursor(8, 122);
    canvas.print("OK SAVE  BKSP DEL  GO CANCEL");
    canvas.pushSprite(0, 0);
}

uint32_t nextUtf8Codepoint(const std::string& text, size_t& i);
void drawMusicUtf8Line(int x, int y, const std::string& text, int max_cols,
                       bool animate, uint16_t color, uint16_t background);

void drawMusicList()
{
    canvas.fillScreen(uiBg());
    drawCyberAccent();
    canvas.setTextSize(2);
    canvas.setTextColor(uiFg(), uiBg());
    canvas.setCursor(8, 8);
    canvas.printf("LISTEN %d/%d", tracks.empty() ? 0 : selected_track + 1, static_cast<int>(tracks.size()));
    canvas.setTextSize(1);
    canvas.setTextColor(uiAccent(), uiBg());
    canvas.setCursor(160, 14);
    canvas.printf("SHUF:%s", shuffle_on ? "ON" : "OFF");
    canvas.setTextSize(2);
    if (tracks.empty()) {
        canvas.setCursor(8, 42);
        canvas.println(sd_ready ? "No MP3" : "No SD");
        canvas.setTextSize(1);
        canvas.setCursor(8, 76);
        canvas.printf("%s R%lu M%lu", music_scan_status.c_str(),
                      static_cast<unsigned long>(music_raw_entries),
                      static_cast<unsigned long>(music_mp3_entries));
    } else {
        int start = std::max(0, selected_track - 1);
        start = std::min(start, std::max(0, static_cast<int>(tracks.size()) - 3));
        int end = std::min(static_cast<int>(tracks.size()), start + 3);
        for (int i = start; i < end; ++i) {
            const int y = 38 + (i - start) * 24;
            const bool selected = i == selected_track;
            const uint16_t fg = selected ? uiBg() : uiFg();
            const uint16_t bg = selected ? uiFg() : uiBg();
            if (selected) canvas.fillRect(4, y - 2, SCREEN_W - 8, 22, bg);
            const char marker = i == selected_track ? '>' : (musicStorageNameNeedsImport(tracks[i]) ? '!' : ' ');
            canvas.setTextSize(2);
            canvas.setTextColor(fg, bg);
            canvas.setCursor(8, y);
            canvas.print(marker);
            drawMusicUtf8Line(28, y, musicDisplayName(tracks[i]), 16, selected, fg, bg);
        }
    }
    canvas.setTextSize(1);
    canvas.setTextColor(uiDim(), uiBg());
    canvas.setCursor(8, 122);
    canvas.print("OK PLAY  2 INFO  1 SHUF");
    canvas.pushSprite(0, 0);
}

void drawMusicInfo()
{
    canvas.fillScreen(uiBg());
    drawCyberAccent();
    canvas.setTextSize(2);
    canvas.setTextColor(uiFg(), uiBg());
    canvas.setCursor(8, 8);
    canvas.println("TRACK INFO");
    if (tracks.empty() || music_info_file.empty()) {
        canvas.setCursor(8, 42);
        canvas.println("No track");
    } else {
        drawMusicUtf8Line(8, 36, music_info_title, 18, true, uiFg(), uiBg());
        canvas.setTextSize(1);
        canvas.setTextColor(uiDim(), uiBg());
        canvas.setCursor(8, 62);
        canvas.printf("FILE %.20s", music_info_file.c_str());
        canvas.setCursor(8, 76);
        canvas.printf("SIZE %.18s", music_info_size.c_str());
        canvas.setCursor(8, 90);
        canvas.printf("STATUS %.16s", music_info_status.c_str());
        canvas.setCursor(8, 104);
        canvas.printf("INDEX %d/%d", music_info_index, music_info_total);
    }
    canvas.setTextSize(1);
    canvas.setTextColor(uiDim(), uiBg());
    canvas.setCursor(8, 122);
    canvas.print("OK PLAY  2 PROBE  GO BACK");
    canvas.pushSprite(0, 0);
}

void drawMusicPlaying()
{
    canvas.fillScreen(uiBg());
    drawCyberAccent();
    canvas.setTextSize(2);
    canvas.setTextColor(uiFg(), uiBg());
    canvas.setCursor(8, 8);
    canvas.println("LISTENING");
    std::string play_label = !override_music_path.empty() ? baseName(override_music_path) : (tracks.empty() ? "" : musicDisplayName(tracks[selected_track]));
    drawMusicUtf8Line(8, 34, play_label, 18, true, uiFg(), uiBg());
    canvas.setCursor(8, 58);
    canvas.setTextColor(uiAccent(), uiBg());
    canvas.printf("VOL:%s", volumeName());
    canvas.setCursor(118, 58);
    canvas.printf("SHUF:%s", shuffle_on ? "ON" : "OFF");
    if (music_underruns > 0) {
        canvas.setTextSize(1);
        canvas.setCursor(202, 62);
        canvas.printf("U%d", music_underruns);
        canvas.setTextSize(2);
    }
    drawWaveform(pcm_chunk, pcm_channels);
    canvas.setTextSize(1);
    canvas.setTextColor(uiDim(), uiBg());
    canvas.setCursor(8, 122);
    canvas.print("OK/GO STOP  UP/DN VOL  L/R TRACK");
    canvas.pushSprite(0, 0);
}

void drawNotesList()
{
    canvas.fillScreen(uiBg());
    drawCyberAccent();
    canvas.setTextSize(2);
    canvas.setTextColor(uiFg(), uiBg());
    canvas.setCursor(8, 8);
    if (notes_cursor == 0) canvas.print(notes.empty() ? "NOTES 0/0" : "NOTES NEW");
    else canvas.printf("WRITE %d/%d", notes_cursor, static_cast<int>(notes.size()));
    canvas.setTextSize(1);
    canvas.setTextColor(uiAccent(), uiBg());
    canvas.setCursor(166, 14);
    canvas.print("TXT");
    canvas.setTextSize(2);
    const int total = static_cast<int>(notes.size()) + 1;
    int rows = 4;
    int start = std::max(0, notes_cursor - 1);
    start = std::min(start, std::max(0, total - rows));
    int end = std::min(total, start + rows);
    for (int i = start; i < end; ++i) {
        canvas.setCursor(8, 34 + (i - start) * 21);
        canvas.setTextColor(i == notes_cursor ? uiBg() : uiFg(), i == notes_cursor ? uiFg() : uiBg());
        if (i == 0) canvas.printf("%c NEW NOTE", i == notes_cursor ? '>' : ' ');
        else {
            std::string label = notes[i - 1];
            if (i == notes_cursor) label = marqueeText(label, 13);
            canvas.printf("%c %.13s", i == notes_cursor ? '>' : ' ', label.c_str());
        }
    }
    if (notes.empty()) {
        canvas.setTextSize(1);
        canvas.setTextColor(uiDim(), uiBg());
        canvas.setCursor(8, 96);
        canvas.print("No notes yet");
    }
    canvas.setTextSize(1);
    canvas.setTextColor(uiDim(), uiBg());
    canvas.setCursor(8, 122);
    canvas.print("OK OPEN 1 NEW BKSP DEL GO");
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

bool isHebrewCodepoint(uint32_t cp)
{
    return cp >= 0x05D0 && cp <= 0x05EA;
}

const char** hebrewBitmap(uint32_t cp)
{
    static const char* Alef[]  = {"10001","01010","00100","01010","10001","10001","00001"};
    static const char* Bet[]   = {"11110","00001","00001","11111","10001","10001","11111"};
    static const char* Gimel[] = {"00011","00001","00001","00001","00001","10001","01110"};
    static const char* Dalet[] = {"11111","00001","00001","00001","00001","00001","00001"};
    static const char* He[]    = {"11111","00001","00001","10001","10001","10001","10001"};
    static const char* Vav[]   = {"00111","00001","00001","00001","00001","00001","00001"};
    static const char* Zayin[] = {"11111","00010","00100","00100","00100","00100","00100"};
    static const char* Het[]   = {"11111","10001","10001","10001","10001","10001","10001"};
    static const char* Tet[]   = {"11111","10001","10101","10101","10001","10001","01110"};
    static const char* Yod[]   = {"00111","00001","00001","00000","00000","00000","00000"};
    static const char* Kaf[]   = {"11111","00001","00001","00001","00001","00001","11111"};
    static const char* Lamed[] = {"00100","00100","11100","10100","10100","10010","10001"};
    static const char* Mem[]   = {"11111","10001","10101","10101","10101","10001","10001"};
    static const char* Nun[]   = {"00111","00001","00001","00001","00001","00001","00111"};
    static const char* Samekh[]= {"11111","10001","10001","10001","10001","10001","11111"};
    static const char* Ayin[]  = {"10001","10001","01001","00101","00011","00001","00001"};
    static const char* Pe[]    = {"11111","10001","10101","00001","00001","00001","11111"};
    static const char* Tsadi[] = {"10001","01001","00101","00011","00001","10001","01110"};
    static const char* Qof[]   = {"11111","10001","10001","10001","10101","00101","00111"};
    static const char* Resh[]  = {"11111","00001","00001","00001","00001","00001","00001"};
    static const char* Shin[]  = {"10101","10101","10101","10101","10001","10001","11111"};
    static const char* Tav[]   = {"11111","10001","10001","10001","10001","10001","10001"};
    switch (cp) {
        case 0x05D0: return Alef;
        case 0x05D1: return Bet;
        case 0x05D2: return Gimel;
        case 0x05D3: return Dalet;
        case 0x05D4: return He;
        case 0x05D5: return Vav;
        case 0x05D6: return Zayin;
        case 0x05D7: return Het;
        case 0x05D8: return Tet;
        case 0x05D9: return Yod;
        case 0x05DA: case 0x05DB: return Kaf;
        case 0x05DC: return Lamed;
        case 0x05DD: case 0x05DE: return Mem;
        case 0x05DF: case 0x05E0: return Nun;
        case 0x05E1: return Samekh;
        case 0x05E2: return Ayin;
        case 0x05E3: case 0x05E4: return Pe;
        case 0x05E5: case 0x05E6: return Tsadi;
        case 0x05E7: return Qof;
        case 0x05E8: return Resh;
        case 0x05E9: return Shin;
        case 0x05EA: return Tav;
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
    canvas.setTextColor(uiFg(), uiBg());
    for (size_t i = 0; i < text.size();) {
        size_t before = i;
        uint32_t cp = nextUtf8Codepoint(text, i);
        if (cp == ' ') { cx += 6 * scale; continue; }
        const char** glyph = cyrillicBitmap(cp);
        if (glyph) {
            drawBitmapGlyph(cx, y + 2, scale, glyph, uiFg());
            cx += 6 * scale;
        } else if (cp < 128) {
            char b[2] = {static_cast<char>(cp), 0};
            canvas.setCursor(cx, y);
            canvas.print(b);
            cx += 6 * scale;
        } else {
            canvas.drawRect(cx, y + 2, 4 * scale, 7 * scale, uiFg());
            cx += 6 * scale;
        }
        if (i == before) ++i;
        if (cx > SCREEN_W - 8) break;
    }
}

void drawMusicUtf8Line(int x, int y, const std::string& text, int max_cols,
                       bool animate, uint16_t color, uint16_t background)
{
    std::vector<uint32_t> glyphs;
    glyphs.reserve(text.size());
    for (size_t i = 0; i < text.size();) glyphs.push_back(nextUtf8Codepoint(text, i));

    // The canvas is left-to-right. Reverse only contiguous Hebrew runs so a
    // title such as "שלום.mp3" keeps its Latin extension in the right order.
    for (size_t begin = 0; begin < glyphs.size();) {
        if (!isHebrewCodepoint(glyphs[begin])) { ++begin; continue; }
        size_t end = begin + 1;
        while (end < glyphs.size() && isHebrewCodepoint(glyphs[end])) ++end;
        std::reverse(glyphs.begin() + begin, glyphs.begin() + end);
        begin = end;
    }

    if (max_cols <= 0) return;
    std::vector<uint32_t> visible;
    visible.reserve(max_cols);
    if (glyphs.size() <= static_cast<size_t>(max_cols)) {
        visible = glyphs;
    } else if (animate) {
        std::vector<uint32_t> loop = glyphs;
        loop.insert(loop.end(), 3, static_cast<uint32_t>(' '));
        const size_t offset = (M5.millis() / 180) % loop.size();
        for (int col = 0; col < max_cols; ++col) visible.push_back(loop[(offset + col) % loop.size()]);
    } else {
        visible.assign(glyphs.begin(), glyphs.begin() + max_cols);
    }

    constexpr int scale = 2;
    int cx = x;
    canvas.setTextSize(scale);
    canvas.setTextColor(color, background);
    for (uint32_t cp : visible) {
        if (cp == ' ') {
            cx += 6 * scale;
            continue;
        }
        const char** bitmap = cyrillicBitmap(cp);
        if (!bitmap) bitmap = hebrewBitmap(cp);
        if (bitmap) {
            drawBitmapGlyph(cx, y + 2, scale, bitmap, color);
        } else {
            char ascii[2] = {static_cast<char>(cp < 128 ? cp : '?'), 0};
            canvas.setCursor(cx, y);
            canvas.print(ascii);
        }
        cx += 6 * scale;
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
    canvas.fillScreen(uiBg());
    drawCyberAccent();
    canvas.setTextSize(1);
    canvas.setTextColor(uiAccent(), uiBg());
    canvas.setCursor(8, 5);
    canvas.printf("%.12s %d/%d", active_note_name.c_str(), reader_lines.empty() ? 0 : reader_scroll + 1, static_cast<int>(reader_lines.size()));
    canvas.setTextSize(2);
    canvas.setTextColor(uiFg(), uiBg());
    if (reader_lines.empty()) {
        canvas.setCursor(8, 48);
        canvas.print("EMPTY");
    } else {
        for (int row = 0; row < READER_LINES_PER_PAGE; ++row) {
            int idx = reader_scroll + row;
            if (idx >= static_cast<int>(reader_lines.size())) break;
            drawTextLineSmart(8, 22 + row * 24, reader_lines[idx]);
        }
    }
    canvas.setTextSize(1);
    canvas.setTextColor(uiDim(), uiBg());
    canvas.setCursor(8, 122);
    canvas.print("UP/DN LINE L/R PAGE 1 EDIT GO LIST");
    canvas.pushSprite(0, 0);
}

void drawNotesEdit()
{
    canvas.fillScreen(uiBg());
    drawCyberAccent();
    canvas.setTextSize(2);
    canvas.setTextColor(uiFg(), uiBg());
    canvas.setCursor(8, 8);
    canvas.println(note_edit_existing ? "EDIT NOTE" : "NEW NOTE");
    canvas.setTextSize(1);
    canvas.setTextColor(uiAccent(), uiBg());
    canvas.setCursor(8, 34);
    canvas.print("LAT / translit text");
    canvas.setTextSize(2);
    canvas.setTextColor(uiFg(), uiBg());
    std::string display_text = note_input;
    std::string tail = utf8TailByChars(display_text, 57);
    std::vector<std::string> lines = wrapUtf8TextColumns(tail, 19);
    int start = std::max(0, static_cast<int>(lines.size()) - 3);
    for (int row = 0; row < 3; ++row) {
        canvas.setCursor(8, 52 + row * 22);
        int idx = start + row;
        if (idx < static_cast<int>(lines.size())) canvas.print(lines[idx].c_str());
    }
    canvas.setTextSize(1);
    canvas.setTextColor(uiDim(), uiBg());
    canvas.setCursor(8, 112);
    canvas.printf("TXT %d/512", static_cast<int>(note_input.size()));
    canvas.setCursor(8, 122);
    canvas.print("OK SAVE          GO CANCEL");
    canvas.pushSprite(0, 0);
}

void drawNotesDeleteConfirm()
{
    canvas.fillScreen(uiBg());
    drawCyberAccent();
    canvas.setTextSize(2);
    canvas.setTextColor(uiAccent(), uiBg());
    canvas.setCursor(8, 8);
    canvas.print("DELETE?");
    canvas.setTextColor(uiFg(), uiBg());
    canvas.setCursor(8, 42);
    if (notes_cursor > 0 && notes_cursor <= static_cast<int>(notes.size())) {
        canvas.printf("%.13s", notes[notes_cursor - 1].c_str());
    } else {
        canvas.print("NO NOTE");
    }
    canvas.setTextSize(1);
    canvas.setTextColor(uiDim(), uiBg());
    canvas.setCursor(8, 88);
    canvas.print("file only, no undo");
    canvas.setCursor(8, 122);
    canvas.print("OK DELETE       GO KEEP");
    canvas.pushSprite(0, 0);
}


void drawRecorderList()
{
    canvas.fillScreen(uiBg());
    drawCyberAccent();
    canvas.setTextSize(2);
    canvas.setTextColor(uiFg(), uiBg());
    canvas.setCursor(8, 8);
    if (recorder_cursor == 0) canvas.print(recordings.empty() ? "REC 0/0" : "REC NEW");
    else canvas.printf("VOICE %d/%d", recorder_cursor, static_cast<int>(recordings.size()));
    canvas.setTextSize(1);
    canvas.setTextColor(uiAccent(), uiBg());
    canvas.setCursor(158, 14);
    canvas.printf("MAX%lus", static_cast<unsigned long>(REC_RECORD_SECONDS));

    const int total = static_cast<int>(recordings.size()) + 1;
    int start = std::max(0, recorder_cursor - 1);
    start = std::min(start, std::max(0, total - 3));
    int end = std::min(total, start + 3);
    for (int i = start; i < end; ++i) {
        canvas.setTextSize(2);
        const int row_y = 36 + (i - start) * 28;
        canvas.setCursor(8, row_y);
        canvas.setTextColor(i == recorder_cursor ? uiBg() : uiFg(), i == recorder_cursor ? uiFg() : uiBg());
        if (i == 0) {
            canvas.printf("%c NEW REC", i == recorder_cursor ? '>' : ' ');
            canvas.setTextSize(1);
            canvas.setTextColor(uiDim(), uiBg());
            canvas.setCursor(28, row_y + 18);
            canvas.print("OK start short / 2 long");
        } else {
            std::string label = recordings[i - 1];
            if (i == recorder_cursor) label = marqueeText(label, 11);
            canvas.printf("%c %.11s", i == recorder_cursor ? '>' : ' ', label.c_str());
            canvas.setTextSize(1);
            canvas.setTextColor(uiDim(), uiBg());
            canvas.setCursor(28, row_y + 18);
            canvas.printf("%.24s", recordingMetaLine(recordings[i - 1]).c_str());
        }
    }
    if (recordings.empty()) {
        canvas.setTextSize(1);
        canvas.setTextColor(uiDim(), uiBg());
        canvas.setCursor(8, 104);
        canvas.print("No recordings yet");
    }
    canvas.setTextSize(1);
    canvas.setTextColor(uiDim(), uiBg());
    canvas.setCursor(8, 122);
    canvas.print("OK PLAY/NEW     BKSP DEL");
    canvas.pushSprite(0, 0);
}

void drawRecorderDeleteConfirm()
{
    canvas.fillScreen(uiBg());
    drawCyberAccent();
    canvas.setTextSize(2);
    canvas.setTextColor(uiAccent(), uiBg());
    canvas.setCursor(8, 8);
    canvas.print("DELETE REC?");
    canvas.setTextColor(uiFg(), uiBg());
    canvas.setCursor(8, 42);
    canvas.printf("%.14s", pending_delete_name.c_str());
    canvas.setTextSize(1);
    canvas.setTextColor(uiDim(), uiBg());
    canvas.setCursor(8, 72);
    canvas.printf("%.28s", recordingMetaLine(pending_delete_name).c_str());
    canvas.setCursor(8, 92);
    canvas.print("No undo");
    canvas.setCursor(8, 122);
    canvas.print("OK DELETE       GO KEEP");
    canvas.pushSprite(0, 0);
}

void drawRecorderRecording()
{
    canvas.fillScreen(uiBg());
    drawCyberAccent();
    canvas.setTextSize(2);
    canvas.setTextColor(uiFg(), uiBg());
    canvas.setCursor(8, 8);
    canvas.println("VOICE REC");
    canvas.setCursor(8, 34);
    canvas.printf("%.14s", active_recording_name.c_str());
    canvas.setTextSize(1);
    canvas.setTextColor(uiDim(), uiBg());
    canvas.setCursor(158, 38);
    canvas.printf("REQ %lus CAP %lus", static_cast<unsigned long>(rec_requested_seconds), static_cast<unsigned long>(rec_target_seconds));
    canvas.setTextSize(2);
    canvas.setCursor(8, 58);
    canvas.setTextColor(uiAccent(), uiBg());
    if (rec_save_pending) {
        canvas.print("SAVING");
        canvas.setTextSize(1);
        canvas.setCursor(8, 84);
        const uint32_t remaining = rec_save_due_ms > M5.millis() ? rec_save_due_ms - M5.millis() : 0;
        canvas.printf("SD SETTLE %lus", static_cast<unsigned long>((remaining + 999) / 1000));
    } else if (rec_write_error) {
        canvas.print("WRITE ERR");
    } else {
        char pcm_dur[12];
        char clk_dur[12];
        char delta_dur[12];
        const uint32_t pcm_ms = recSecondsMsFromSamples(rec_samples_written);
        const uint32_t clk_ms = (M5.millis() - rec_started_ms);
        const int32_t delta_ms = recClockDeltaMs();
        formatTenthsSeconds(pcm_ms, pcm_dur, sizeof(pcm_dur));
        formatTenthsSeconds(clk_ms, clk_dur, sizeof(clk_dur));
        formatTenthsSeconds((delta_ms < 0) ? -delta_ms : delta_ms, delta_dur, sizeof(delta_dur));
        canvas.printf("PCM:%s", pcm_dur);
        canvas.setTextSize(1);
        canvas.setCursor(8, 82);
        canvas.printf("CLK %s  MAX %lus", clk_dur, static_cast<unsigned long>(std::max<size_t>(REC_MIN_SECONDS, rec_capture_capacity / rec_record_sample_rate)));
        canvas.setCursor(8, 94);
        if (delta_ms >= 0) {
            canvas.printf("DIFF +%s", delta_dur);
        } else {
            canvas.printf("DIFF -%s", delta_dur);
        }
        canvas.setCursor(130, 94);
        canvas.printf(" %s", rec_auto_stopped ? "AUTO" : "MAN");
        canvas.setCursor(8, 106);
        canvas.printf("CH %lu  TAKE %lu", static_cast<unsigned long>(rec_mic_chunks), static_cast<unsigned long>(rec_last_take));
    }
    drawWaveform(pcm_chunk, 1);
    canvas.setTextSize(1);
    canvas.setTextColor(uiDim(), uiBg());
    canvas.setCursor(8, 122);
    canvas.print("OK STOP/SAVE   GO SAVE");
    canvas.pushSprite(0, 0);
}

void drawRecorderPlaying()
{
    canvas.fillScreen(uiBg());
    drawCyberAccent();
    canvas.setTextSize(2);
    canvas.setTextColor(uiFg(), uiBg());
    canvas.setCursor(8, 8);
    canvas.println("PLAY REC");
    canvas.setCursor(8, 34);
    canvas.printf("%.14s", active_recording_name.c_str());
    canvas.setCursor(8, 58);
    canvas.setTextColor(uiAccent(), uiBg());
    canvas.printf("CHUNK:%lu", static_cast<unsigned long>(rec_play_chunks));
    drawWaveform(pcm_chunk, 1);
    canvas.setTextSize(1);
    canvas.setTextColor(uiDim(), uiBg());
    canvas.setCursor(8, 122);
    canvas.print("OK/GO STOP");
    canvas.pushSprite(0, 0);
}

void drawReaderList()
{
    canvas.fillScreen(uiBg());
    drawCyberAccent();
    canvas.setTextSize(2);
    canvas.setTextColor(uiFg(), uiBg());
    canvas.setCursor(8, 8);
    canvas.printf("BOOKS %d/%d", books.empty() ? 0 : selected_book + 1, static_cast<int>(books.size()));
    canvas.setTextSize(1);
    canvas.setTextColor(uiAccent(), uiBg());
    canvas.setCursor(166, 14);
    canvas.print("TXT");
    canvas.setTextSize(2);
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
            canvas.setTextColor(i == selected_book ? uiBg() : uiFg(), i == selected_book ? uiFg() : uiBg());
            std::string label = books[i];
            if (i == selected_book) label = marqueeText(label, 12);
            const bool bookmarked = reader_bookmarks.count(books[i]) || reader_stream_bookmarks.count(books[i]);
            canvas.printf("%c%c%.12s", i == selected_book ? '>' : ' ', bookmarked ? '*' : ' ', label.c_str());
        }
    }
    canvas.setTextSize(1);
    canvas.setTextColor(uiDim(), uiBg());
    canvas.setCursor(8, 122);
    canvas.print("OK READ  *BMK  GO BACK");
    canvas.pushSprite(0, 0);
}

void drawReaderView()
{
    canvas.fillScreen(uiBg());
    drawCyberAccent();
    canvas.setTextSize(1);
    canvas.setTextColor(uiAccent(), uiBg());
    canvas.setCursor(8, 5);
    if (reader_streaming) {
        int pct = reader_stream_file_size ? static_cast<int>(reader_stream_offset * 100 / reader_stream_file_size) : 0;
        canvas.printf("%.14s %d%%", active_book_name.c_str(), pct);
    } else {
        canvas.printf("%.14s %d/%d", active_book_name.c_str(), reader_lines.empty() ? 0 : reader_scroll + 1, static_cast<int>(reader_lines.size()));
    }
    canvas.setTextSize(2);
    canvas.setTextColor(uiFg(), uiBg());
    if (reader_streaming) {
        for (int row = 0; row < READER_LINES_PER_PAGE; ++row) {
            if (row >= static_cast<int>(reader_stream_lines.size())) break;
            drawTextLineSmart(8, 22 + row * 24, reader_stream_lines[row]);
        }
    } else if (reader_lines.empty()) {
        canvas.setCursor(8, 48);
        canvas.print("EMPTY");
    } else {
        for (int row = 0; row < READER_LINES_PER_PAGE; ++row) {
            int idx = reader_scroll + row;
            if (idx >= static_cast<int>(reader_lines.size())) break;
            drawTextLineSmart(8, 22 + row * 24, reader_lines[idx]);
        }
    }
    canvas.setTextSize(1);
    canvas.setTextColor(uiDim(), uiBg());
    canvas.setCursor(8, 122);
    if (!reader_stream_status.empty()) canvas.printf("%.36s", reader_stream_status.c_str());
    else canvas.print("UP/DN LINE L/R PAGE 1 SPD GO LIST");
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
    if (reader_streaming) return reader_stream_speed_text;
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
    canvas.setTextColor(color, uiBg());
    canvas.setCursor(x, y);
    canvas.print(text.c_str());
}

void drawReaderSpeed()
{
    canvas.fillScreen(uiBg());
    drawCyberAccent();
    canvas.setTextSize(2);
    canvas.setTextColor(uiFg(), uiBg());
    canvas.setCursor(8, 6);
    canvas.print("SPEED");
    canvas.setCursor(92, 6);
    canvas.printf("%s", speedModeName());
    canvas.setCursor(8, 30);
    canvas.setTextColor(uiAccent(), uiBg());
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
        drawCenteredText(text, size == 3 ? 62 : 66, size, speed_paused ? uiDim() : uiFg());
    }

    canvas.setTextSize(1);
    canvas.setTextColor(uiDim(), uiBg());
    canvas.setCursor(8, 106);
    if (reader_streaming) {
        int pct = reader_stream_file_size ? static_cast<int>(reader_stream_speed_offset * 100 / reader_stream_file_size) : 0;
        canvas.printf("POS %d%%", pct);
    } else {
        int total = speed_mode == SpeedMode::Line ? static_cast<int>(reader_lines.size()) : static_cast<int>(reader_words.size());
        canvas.printf("POS %d/%d", std::min(speed_index + 1, std::max(1, total)), total);
    }
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
    canvas.setTextColor(uiFg(), uiBg());
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

void applyTimerPreset()
{
    timer_seconds = TIMER_PRESETS[timer_preset_index];
    timer_remaining_ms = timer_seconds * 1000UL;
    timer_running = false;
    timer_done = false;
    alert_until_ms = 0;
}

int timerPresetMinutes()
{
    for (size_t i = 0; i < sizeof(TIMER_PRESETS) / sizeof(TIMER_PRESETS[0]); ++i) {
        if (timer_seconds == TIMER_PRESETS[i]) return TIMER_PRESETS[i] / 60;
    }
    return 0;
}

void startAlert(uint32_t ms)
{
    alert_until_ms = M5.millis() + ms;
    last_alert_beep_ms = 0;
    M5.Speaker.begin();
    M5.Speaker.setVolume(soundVolume());
}

void updateAlert()
{
    uint32_t now = M5.millis();
    if (alert_until_ms == 0 || now > alert_until_ms || sound_mode == SoundMode::Off) return;
    if (last_alert_beep_ms == 0 || now - last_alert_beep_ms > 650) {
        M5.Speaker.setVolume(soundVolume());
        M5.Speaker.tone(880, 160);
        last_alert_beep_ms = now;
    }
}

void drawTimeApp()
{
    canvas.fillScreen(uiBg());
    drawCyberAccent();
    canvas.setTextSize(2);
    canvas.setTextColor(uiFg(), uiBg());
    canvas.setCursor(8, 6);
    canvas.print("TIME");
    canvas.setCursor(88, 6);
    const char* mode = time_mode == TimeMode::Clock ? "CLOCK" : (time_mode == TimeMode::Stopwatch ? "STOP" : (time_mode == TimeMode::Timer ? "TIMER" : "ALARM"));
    canvas.setTextColor(uiAccent(), uiBg());
    canvas.print(mode);

    char buf[16];
    if (time_mode == TimeMode::Clock) {
        formatHMS(elapsedClockSeconds(), buf, sizeof(buf));
        drawBigTime(buf, 48);
        canvas.setTextSize(1);
        canvas.setTextColor(uiAccent(), uiBg());
        canvas.setCursor(8, 96);
        canvas.printf("SET:%s  1 FIELD", timeFieldName());
    } else if (time_mode == TimeMode::Stopwatch) {
        uint32_t ms = stopwatchDisplayMs();
        uint32_t sec = ms / 1000;
        snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu.%lu", static_cast<unsigned long>(sec / 3600), static_cast<unsigned long>((sec / 60) % 60), static_cast<unsigned long>(sec % 60), static_cast<unsigned long>((ms / 100) % 10));
        canvas.setTextSize(2);
        canvas.setTextColor(uiFg(), uiBg());
        canvas.setCursor(8, 52);
        canvas.print(buf);
        canvas.setTextSize(1);
        canvas.setTextColor(stopwatch_running ? uiAccent() : uiDim(), uiBg());
        canvas.setCursor(8, 96);
        canvas.print(stopwatch_running ? "RUN" : "PAUSE");
    } else if (time_mode == TimeMode::Timer) {
        uint32_t sec = (timerDisplayMs() + 999) / 1000;
        snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", static_cast<unsigned long>(sec / 3600), static_cast<unsigned long>((sec / 60) % 60), static_cast<unsigned long>(sec % 60));
        drawBigTime(buf, 48);
        canvas.setTextSize(1);
        canvas.setTextColor((timer_done || timer_running) ? uiAccent() : uiDim(), uiBg());
        canvas.setCursor(8, 96);
        int preset_min = timerPresetMinutes();
        if (preset_min > 0) canvas.printf("%s PRESET:%dm", timer_done ? "DONE" : (timer_running ? "RUN" : "SET"), preset_min);
        else canvas.printf("%s CUSTOM", timer_done ? "DONE" : (timer_running ? "RUN" : "SET"));
    } else {
        formatHMS(alarm_seconds, buf, sizeof(buf));
        drawBigTime(buf, 48);
        canvas.setTextSize(1);
        canvas.setTextColor((alarm_enabled || alarm_ringing) ? uiAccent() : uiDim(), uiBg());
        canvas.setCursor(8, 96);
        canvas.printf("%s SET:%s", alarm_ringing ? "RING" : (alarm_enabled ? "ON" : "OFF"), timeFieldName());
    }
    canvas.setTextSize(1);
    canvas.setTextColor(uiDim(), uiBg());
    canvas.setCursor(8, 122);
    canvas.print(time_mode == TimeMode::Timer ? "OK START  1 PRESET  L/R MODE" : "OK ON/START  1 FIELD/RST  L/R MODE");
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
        appendInboxEvent("TIMER", "done");
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
    canvas.fillScreen(uiBg());
    drawCyberAccent();
    canvas.setTextSize(2);
    canvas.setTextColor(uiFg(), uiBg());
    canvas.setCursor(8, 8);
    canvas.printf("FILES %d/%d", file_entries.empty() ? 0 : files_cursor + 1, static_cast<int>(file_entries.size()));
    canvas.setTextSize(1);
    canvas.setTextColor(uiAccent(), uiBg());
    canvas.setCursor(8, 30);
    std::string shown_path = filesDisplayPath(files_path);
    canvas.printf("%.28s", shown_path.c_str());
    uint64_t total = 0, free_b = 0;
    canvas.setCursor(8, 40);
    if (sdUsage(&total, &free_b) && total >= free_b) {
        canvas.printf("SD %s FREE USED %s", formatBytes(free_b).c_str(), formatBytes(total - free_b).c_str());
    } else {
        canvas.print("SD -- FREE USED --");
    }
    if (file_entries.empty()) {
        canvas.setTextSize(2);
        canvas.setTextColor(uiFg(), uiBg());
        canvas.setCursor(8, 62);
        canvas.print(files_status.c_str());
        canvas.setTextSize(1);
        canvas.setTextColor(uiDim(), uiBg());
        canvas.setCursor(8, 90);
        canvas.print("1 ROOT  GO BACK");
    } else {
        int rows = 3;
        int start = std::max(0, files_cursor - 1);
        start = std::min(start, std::max(0, static_cast<int>(file_entries.size()) - rows));
        int end = std::min(static_cast<int>(file_entries.size()), start + rows);
        canvas.setTextSize(2);
        for (int i = start; i < end; ++i) {
            const auto& e = file_entries[i];
            canvas.setCursor(8, 58 + (i - start) * 20);
            canvas.setTextColor(i == files_cursor ? uiBg() : uiFg(), i == files_cursor ? uiFg() : uiBg());
            std::string label = e.name;
            if (i == files_cursor) label = marqueeText(label, 12);
            canvas.printf("%c%c%.12s", i == files_cursor ? '>' : ' ', e.is_dir ? '/' : ' ', label.c_str());
        }
    }
    canvas.setTextSize(1);
    canvas.setTextColor(uiDim(), uiBg());
    canvas.setCursor(8, 122);
    canvas.print("OK OPEN  BKSP DEL  2 TEST");
    canvas.pushSprite(0, 0);
}

void drawFilesDeleteConfirm()
{
    canvas.fillScreen(uiBg());
    drawCyberAccent();
    canvas.setTextSize(2);
    canvas.setTextColor(uiAccent(), uiBg());
    canvas.setCursor(8, 8);
    canvas.print("DELETE?");
    canvas.setTextColor(uiFg(), uiBg());
    canvas.setCursor(8, 42);
    canvas.printf("%.14s", pending_delete_name.c_str());
    canvas.setTextSize(1);
    canvas.setTextColor(uiDim(), uiBg());
    struct stat st = {};
    canvas.setCursor(8, 70);
    if (!pending_delete_path.empty() && stat(pending_delete_path.c_str(), &st) == 0) {
        canvas.printf("SIZE %s", formatBytes(static_cast<uint64_t>(st.st_size)).c_str());
    } else {
        canvas.print("SIZE ?");
    }
    canvas.setCursor(8, 84);
    std::string rel = filesDisplayPath(pending_delete_path);
    canvas.printf("PATH %.26s", rel.c_str());
    canvas.setCursor(8, 122);
    canvas.print("OK DELETE       GO KEEP");
    canvas.pushSprite(0, 0);
}

void drawFilesInfo()
{
    canvas.fillScreen(uiBg());
    drawCyberAccent();
    canvas.setTextSize(2);
    canvas.setTextColor(uiFg(), uiBg());
    canvas.setCursor(8, 8);
    canvas.print("FILE INFO");
    canvas.setTextSize(1);
    canvas.setTextColor(uiAccent(), uiBg());
    canvas.setCursor(8, 32);
    canvas.printf("NAME %.24s", file_info_entry.name.c_str());
    canvas.setCursor(8, 46);
    canvas.printf("TYPE %s", fileTypeLabel(file_info_entry).c_str());
    canvas.setCursor(8, 60);
    canvas.printf("SIZE %s", file_info_entry.is_dir ? "-" : formatBytes(file_info_entry.size).c_str());
    canvas.setCursor(8, 74);
    std::string rel = filesDisplayPath(file_info_entry.path);
    canvas.printf("PATH %.26s", rel.c_str());
    canvas.setTextColor(uiDim(), uiBg());
    canvas.setCursor(8, 102);
    if (file_info_entry.is_dir) canvas.print("OK opens folder");
    else if (isSupportedOpenFile(file_info_entry)) canvas.print("Known file type");
    else canvas.print("Stored only, not opened");
    canvas.setCursor(8, 122);
    canvas.print("OK/BACK FILES");
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
    if (e.size == 0 || (ext != ".txt" && ext != ".mp3" && ext != ".wav" && ext != ".pcm")) {
        file_info_entry = e;
        screen = Screen::FilesInfo;
        return true;
    }
    if (ext == ".txt") {
        active_book_name = e.name;
        active_note_name = e.name;
        if (e.path.rfind(std::string(BOOKS_DIR) + "/", 0) == 0) {
            scanBooks();
            auto it = std::find(books.begin(), books.end(), e.name);
            if (it == books.end()) {
                if (err) *err = "book not found";
                return false;
            }
            selected_book = static_cast<int>(std::distance(books.begin(), it));
            if (!loadSelectedBook(err)) return false;
            screen = Screen::ReaderView;
            return true;
        }
        if (!loadTextFile(e.path, e.name, err)) return false;
        screen = e.path.rfind(std::string(NOTES_DIR) + "/", 0) == 0 ? Screen::NotesView : Screen::ReaderView;
        return true;
    }
    if (ext == ".mp3") {
        override_music_path = e.path;
        if (!startPlayback(err)) {
            if (err) *err = musicProblemBody(e.path, *err);
            return false;
        }
        return true;
    }
    if (ext == ".wav" || ext == ".pcm") {
        rec_play_file = fopen(e.path.c_str(), "rb");
        if (!rec_play_file) {
            if (err) { *err = "open: "; *err += std::strerror(errno); }
            return false;
        }
        uint32_t data_offset = 0;
        uint32_t sample_rate = REC_SAMPLE_RATE;
        uint16_t bits = 16;
        if (ext == ".wav" && !readWavHeader(rec_play_file, &data_offset, &sample_rate, &bits, err)) {
            fclose(rec_play_file);
            rec_play_file = nullptr;
            return false;
        }
        fseek(rec_play_file, static_cast<long>(data_offset), SEEK_SET);
        rec_buffer.assign(REC_BUFFER_SAMPLES * 4, 0);
        rec_play_chunks = 0;
        rec_play_next_ready = false;
        rec_play_current_last = false;
        rec_play_bits = bits;
        pcm_rate = static_cast<int>(sample_rate);
        pcm_channels = 1;
        active_recording_name = e.name;
        M5.Mic.end();
        M5.Speaker.begin();
        applyVolume();
        screen = Screen::RecorderPlaying;
        return true;
    }
    file_info_entry = e;
    screen = Screen::FilesInfo;
    return true;
}

void drawSettings()
{
    canvas.fillScreen(uiBg());
    canvas.setTextSize(2);
    canvas.setTextColor(uiFg(), uiBg());
    canvas.setCursor(8, 8);
    canvas.print("SETTINGS");
    int start = std::max(0, settings_cursor - 1);
    constexpr int settings_count = 9;
    start = std::min(start, std::max(0, settings_count - 3));
    for (int i = start; i < std::min(settings_count, start + 3); ++i) {
        canvas.setCursor(8, 34 + (i - start) * 23);
        const bool edit_row = settings_cast_editing && (
            (settings_cast_edit_host && i == 4) ||
            (!settings_cast_edit_host && i == 5));
        const bool sel = i == settings_cursor;
        canvas.setTextColor(sel ? uiBg() : uiFg(), sel ? uiFg() : uiBg());
        if (i == 0) canvas.printf("%c THEME %s", sel ? '>' : ' ', themeName());
        else if (i == 1) canvas.printf("%c SOUND %s", sel ? '>' : ' ', soundName());
        else if (i == 2) canvas.printf("%c TIMEOUT %s", sel ? '>' : ' ', timeoutName());
        else if (i == 3) canvas.printf("%c POWER %s", sel ? '>' : ' ', power_save ? "ON" : "OFF");
        else if (i == 4) {
            const char* value = (sel && settings_cast_editing && settings_cast_edit_host) ? settings_cast_edit_buffer.c_str() : cast_host;
            canvas.printf("%c CAST HOST %s", sel ? '>' : ' ', value);
        } else if (i == 5) {
            char port_value[16];
            snprintf(port_value, sizeof(port_value), "%u", static_cast<unsigned int>(cast_port));
            const char* value = (sel && settings_cast_editing && !settings_cast_edit_host) ? settings_cast_edit_buffer.c_str() : port_value;
            if (i == 5 && settings_cast_editing && sel && !settings_cast_edit_host) {
                canvas.printf("%c CAST PORT %s", sel ? '>' : ' ', value);
            } else {
                canvas.printf("%c CAST PORT %u", sel ? '>' : ' ', static_cast<unsigned int>(cast_port));
            }
        } else if (i == 6) canvas.printf("%c SD REPROBE", sel ? '>' : ' ');
        else if (i == 7) canvas.printf("%c ABOUT", sel ? '>' : ' ');
        else if (i == 8) canvas.printf("%c CONNECTIONS", sel ? '>' : ' ');
        if (edit_row) {
            canvas.setTextColor(uiDim(), uiBg());
            canvas.setCursor(8, 106);
            canvas.print("type text, OK to save");
            break;
        }
    }
    canvas.setTextSize(1);
    canvas.setTextColor(uiDim(), uiBg());
    uint64_t total = 0, free_b = 0;
    canvas.setCursor(8, 102);
    if (sdUsage(&total, &free_b) && total >= free_b) {
        canvas.printf("SD FREE %s USED %s", formatBytes(free_b).c_str(), formatBytes(total - free_b).c_str());
    } else {
        canvas.print("SD NOT READY");
    }
    canvas.setCursor(8, 112);
    int level = batteryPercent();
    if (level >= 0) canvas.printf("BAT %d%% PASS %s", level, connection_ap_password);
    else canvas.printf("BAT -- V%d PASS %s", battery_last_mv, connection_ap_password);
    canvas.setCursor(8, 122);
    if (settings_cast_editing) canvas.print("OK SAVE    BS DELETE    GO BACK");
    else canvas.print("OK OPEN/CHANGE       GO BACK");
    canvas.pushSprite(0, 0);
}

void setConnectionStatus(const char* endpoint, const char* err)
{
    if (endpoint && endpoint[0]) snprintf(connection_last_endpoint, sizeof(connection_last_endpoint), "%s", endpoint);
    if (err && err[0]) snprintf(connection_last_error, sizeof(connection_last_error), "%s", err);
    connection_dirty = true;
    dirty = true;
}

bool hexVal(char c, char* out)
{
    if (c >= '0' && c <= '9') { *out = c - '0'; return true; }
    if (c >= 'a' && c <= 'f') { *out = c - 'a' + 10; return true; }
    if (c >= 'A' && c <= 'F') { *out = c - 'A' + 10; return true; }
    return false;
}

std::string urlDecode(const char* text)
{
    std::string out;
    if (!text) return out;
    for (size_t i = 0; text[i] && out.size() < 160; ++i) {
        if (text[i] == '%' && text[i + 1] && text[i + 2]) {
            char hi = 0, lo = 0;
            if (hexVal(text[i + 1], &hi) && hexVal(text[i + 2], &lo)) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(text[i] == '+' ? ' ' : text[i]);
    }
    return out;
}

bool getRequestPath(httpd_req_t* req, std::string* api_path, const char* fallback = nullptr)
{
    char query[192] = {};
    char value[176] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "path", value, sizeof(value)) == ESP_OK) {
        *api_path = urlDecode(value);
    } else if (fallback) {
        *api_path = fallback;
    } else {
        return false;
    }
    return true;
}

bool getQueryValue(httpd_req_t* req, const char* key, char* out, size_t out_len)
{
    char query[224] = {};
    return httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
           httpd_query_key_value(query, key, out, out_len) == ESP_OK;
}

bool getQueryUint(httpd_req_t* req, const char* key, size_t* out)
{
    char value[24] = {};
    if (!getQueryValue(req, key, value, sizeof(value))) return false;
    char* end = nullptr;
    unsigned long parsed = std::strtoul(value, &end, 10);
    if (!end || *end != '\0') return false;
    *out = static_cast<size_t>(parsed);
    return true;
}

bool isAllowedApiPath(const std::string& path)
{
    static const char* roots[] = {"/music", "/books", "/notes", "/rec", "/recs", "/cardputer"};
    if (path == "/") return true;
    for (const char* root : roots) {
        size_t n = std::strlen(root);
        if (path == root || (path.rfind(root, 0) == 0 && path.size() > n && path[n] == '/')) return true;
    }
    return false;
}

bool apiPathToSdPath(const std::string& api_path, std::string* full_path, char* err, size_t err_len)
{
    if (api_path.empty() || api_path[0] != '/') {
        snprintf(err, err_len, "bad path");
        return false;
    }
    if (api_path.find("..") != std::string::npos || api_path.find('\\') != std::string::npos) {
        snprintf(err, err_len, "bad path");
        return false;
    }
    for (char c : api_path) {
        if (static_cast<unsigned char>(c) < 32) {
            snprintf(err, err_len, "bad char");
            return false;
        }
    }
    if (!isAllowedApiPath(api_path)) {
        snprintf(err, err_len, "path blocked");
        return false;
    }
    if (api_path == "/") {
        *full_path = MOUNT_POINT;
    } else if (api_path == "/cardputer") {
        *full_path = CONFIG_DIR;
    } else if (api_path.rfind("/cardputer/", 0) == 0) {
        *full_path = std::string(CONFIG_DIR) + api_path.substr(std::strlen("/cardputer"));
    } else {
        *full_path = std::string(MOUNT_POINT) + api_path;
    }
    return true;
}

bool isSafe83Name(const std::string& name)
{
    if (name.empty() || name == "." || name == ".." || name.size() > 12) return false;
    size_t dot = name.find('.');
    if (dot != std::string::npos && name.find('.', dot + 1) != std::string::npos) return false;
    size_t base_len = dot == std::string::npos ? name.size() : dot;
    size_t ext_len = dot == std::string::npos ? 0 : name.size() - dot - 1;
    if (base_len == 0 || base_len > 8 || ext_len > 3) return false;
    for (char c : name) {
        if (c == '.') continue;
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') continue;
        return false;
    }
    return true;
}

bool appendMusicIndex(const std::string& stored_name, const std::string& original_name)
{
    FILE* f = fopen(MUSIC_INDEX_FILE, "ab");
    if (!f) return false;
    std::string clean = original_name;
    for (char& c : clean) {
        if (c == '\n' || c == '\r' || c == '|') c = '_';
    }
    fprintf(f, "%s|%s\n", stored_name.c_str(), clean.c_str());
    bool ok = fflush(f) == 0;
    if (fclose(f) != 0) ok = false;
    return ok;
}

bool nextMusicUploadName(std::string* stored_api, char* err, size_t err_len)
{
    std::string music_full;
    if (!apiPathToSdPath("/music", &music_full, err, err_len)) {
        return false;
    }
    struct stat pst = {};
    if (stat(music_full.c_str(), &pst) != 0) {
        errno = 0;
        if (mkdir(music_full.c_str(), 0775) != 0 && errno != EEXIST) {
            snprintf(err, err_len, "mkdir %s", std::strerror(errno));
            return false;
        }
    } else if (!S_ISDIR(pst.st_mode)) {
        snprintf(err, err_len, "music not dir");
        return false;
    }
    for (int i = 1; i <= 999; ++i) {
        char name[16];
        snprintf(name, sizeof(name), "M%03d.MP3", i);
        std::string full = std::string(MUSIC_DIR) + "/" + name;
        struct stat st = {};
        if (stat(full.c_str(), &st) != 0) {
            *stored_api = std::string("/music/") + name;
            return true;
        }
    }
    snprintf(err, err_len, "music full");
    return false;
}

bool resolveUploadApiPath(const std::string& requested_api, std::string* stored_api, std::string* original_name, char* err, size_t err_len)
{
    size_t slash = requested_api.find_last_of('/');
    if (slash == std::string::npos || slash == 0) {
        snprintf(err, err_len, "bad upload path");
        return false;
    }
    std::string parent = requested_api.substr(0, slash);
    std::string filename = requested_api.substr(slash + 1);
    *stored_api = requested_api;
    original_name->clear();
    if (isSafe83Name(filename)) return true;
    if (parent == "/music" && hasMp3Ext(filename)) {
        *original_name = filename;
        return nextMusicUploadName(stored_api, err, err_len);
    }
    snprintf(err, err_len, "use 8.3 name");
    return false;
}

bool uploadPathAllowed(const std::string& api_path, std::string* parent_api, std::string* filename, char* err, size_t err_len)
{
    size_t slash = api_path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) {
        snprintf(err, err_len, "bad upload path");
        return false;
    }
    *parent_api = api_path.substr(0, slash);
    *filename = api_path.substr(slash + 1);
    static const char* roots[] = {"/music", "/books", "/notes", "/rec", "/recs", "/cardputer"};
    bool direct_root = false;
    for (const char* root : roots) {
        if (*parent_api == root) {
            direct_root = true;
            break;
        }
    }
    if (!direct_root) {
        snprintf(err, err_len, "root only");
        return false;
    }
    if (!isSafe83Name(*filename)) {
        snprintf(err, err_len, "use 8.3 name");
        return false;
    }
    return true;
}

bool ensureUploadParent(const std::string& parent_api, const std::string& parent_full, char* err, size_t err_len)
{
    if (parent_api == "/cardputer") {
        return ensureConnectionWriteDir(err, err_len);
    }
    struct stat pst = {};
    if (stat(parent_full.c_str(), &pst) != 0) {
        errno = 0;
        if (mkdir(parent_full.c_str(), 0775) != 0 && errno != EEXIST) {
            snprintf(err, err_len, "mkdir %s", std::strerror(errno));
            return false;
        }
    } else if (!S_ISDIR(pst.st_mode)) {
        snprintf(err, err_len, "parent not dir");
        return false;
    }
    return true;
}

bool prepareUploadPath(const std::string& api_path, std::string* full_path, std::string* parent_api, char* err, size_t err_len)
{
    std::string filename, parent_full;
    if (!uploadPathAllowed(api_path, parent_api, &filename, err, err_len) ||
        !apiPathToSdPath(api_path, full_path, err, err_len) ||
        !apiPathToSdPath(*parent_api, &parent_full, err, err_len)) {
        return false;
    }
    if (!initSd()) {
        snprintf(err, err_len, "sd mount failed");
        return false;
    }
    return ensureUploadParent(*parent_api, parent_full, err, err_len);
}

void sendHttpError(httpd_req_t* req, const char* endpoint, const char* reason, httpd_err_code_t code = HTTPD_400_BAD_REQUEST)
{
    setConnectionStatus(endpoint, reason);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send_err(req, code, reason);
}

bool requestCastEndpoint(esp_http_client_method_t method, const char* path, std::string* response, std::string* err)
{
    if (!cast_trace_enabled) cast_retries_total = 0;
    if (!cast_host[0] || !cast_port) {
        if (err) *err = "cast host/port unset";
        setCastTrace("NONE", path ? path : "-", false, 0, 0, err ? err->c_str() : "cast host/port unset");
        return false;
    }
    std::string full_path = (path && path[0]) ? std::string(path) : std::string("/");
    if (!full_path.empty() && full_path[0] != '/') full_path.insert(0, 1, '/');
    char url[160] = {};
    const char* host = cast_host;
    if (!host[0]) host = "192.168.4.1";
    if (snprintf(url, sizeof(url), "http://%s:%u%s", host, static_cast<unsigned int>(cast_port), full_path.c_str()) >= static_cast<int>(sizeof(url))) {
        if (err) *err = "url too long";
        setCastTrace("URL", full_path.c_str(), false, 0, 0, "url too long");
        return false;
    }
    int tries = 0;
    while (tries < 3) {
        uint32_t start_ms = M5.millis();
        esp_http_client_config_t cfg = {};
        cfg.url = url;
        cfg.method = method;
        cfg.timeout_ms = CAST_REQUEST_TIMEOUT_MS;
        cfg.skip_cert_common_name_check = true;
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) {
            if (err) *err = "http init";
            setCastTrace(full_path.c_str(), full_path.c_str(), false, 0, 0, "http init failed");
            return false;
        }
        ++cast_retries_total;
        esp_err_t rc = esp_http_client_perform(client);
        int code = rc == ESP_OK ? esp_http_client_get_status_code(client) : 0;
        uint16_t lat = static_cast<uint16_t>(M5.millis() - start_ms);
        if (rc == ESP_OK && code >= 200 && code < 300) {
            if (response) {
                response->clear();
                char buf[128];
                int total = 0;
                while (true) {
                    int got = esp_http_client_read(client, buf, static_cast<int>(sizeof(buf) - 1));
                    if (got <= 0) break;
                    buf[got] = 0;
                    total += got;
                    if (response->size() < CAST_MAX_TRACE_LINE) {
                        size_t room = CAST_MAX_TRACE_LINE - response->size();
                        response->append(buf, static_cast<size_t>(std::min(room, static_cast<size_t>(got))));
                    }
                    if (total > 8192) break;
                }
            }
            setCastTrace(full_path.c_str(), full_path.c_str(), true, static_cast<uint16_t>(code), lat, "none");
            esp_http_client_cleanup(client);
            if (err) err->clear();
            return true;
        }
        if (err) *err = rc == ESP_OK ? "http error" : esp_err_to_name(rc);
        setCastTrace(full_path.c_str(), full_path.c_str(), false, static_cast<uint16_t>(code), lat, err ? err->c_str() : "failed");
        esp_http_client_cleanup(client);
        vTaskDelay(pdMS_TO_TICKS(150 + tries * 120));
        ++tries;
    }
    return false;
}

[[maybe_unused]] bool requestCastEndpoint(const char* path, std::string* response, std::string* err)
{
    return requestCastEndpoint(HTTP_METHOD_GET, path, response, err);
}

bool requestCastPathWithCompat(esp_http_client_method_t method, const char* primary, const char* fallback, std::string* response, std::string* err)
{
    if (requestCastEndpoint(method, primary, response, err)) return true;
    if (fallback && requestCastEndpoint(method, fallback, response, err)) return true;
    return false;
}

esp_err_t connectionCastStatusHandler(httpd_req_t* req)
{
    ++connection_req_count;
    const char* endpoint = "/api/cast/status";
    std::string response;
    std::string err;
    if (!requestCastPathWithCompat(HTTP_METHOD_GET, "/api/cast/status", "/cast/status", &response, &err)) {
        sendHttpError(req, endpoint, err.empty() ? "cast status" : err.c_str(), HTTPD_500_INTERNAL_SERVER_ERROR);
        return ESP_OK;
    }
    setConnectionStatus(endpoint, "none");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, response.empty() ? "OK CAST STATUS\n" : response.c_str());
}

esp_err_t connectionCastActionHandler(httpd_req_t* req, const char* endpoint, const char* primary, const char* fallback)
{
    ++connection_req_count;
    std::string response;
    std::string err;
    if (!requestCastPathWithCompat(HTTP_METHOD_POST, primary, fallback, &response, &err)) {
        sendHttpError(req, endpoint, err.empty() ? "cast action" : err.c_str(), HTTPD_500_INTERNAL_SERVER_ERROR);
        return ESP_OK;
    }
    setConnectionStatus(endpoint, "none");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, response.empty() ? "OK CAST\n" : response.c_str());
}

esp_err_t connectionCastPlayHandler(httpd_req_t* req)
{
    return connectionCastActionHandler(req, "/api/cast/play", "/api/cast/play", "/cast/play");
}

esp_err_t connectionCastNextHandler(httpd_req_t* req)
{
    return connectionCastActionHandler(req, "/api/cast/next", "/api/cast/next", "/cast/next");
}

esp_err_t connectionCastPrevHandler(httpd_req_t* req)
{
    return connectionCastActionHandler(req, "/api/cast/prev", "/api/cast/prev", "/cast/prev");
}

void cleanupUploadSession(bool remove_partial)
{
    if (connection_upload_file) {
        fflush(connection_upload_file);
        fclose(connection_upload_file);
        connection_upload_file = nullptr;
    }
    if (remove_partial && !connection_upload_part_path.empty()) {
        unlink(connection_upload_part_path.c_str());
    }
    connection_upload_active = false;
    connection_upload_done = 0;
    connection_upload_total = 0;
    connection_upload_session = false;
    connection_upload_path.clear();
    connection_upload_original_name.clear();
    connection_upload_final_path.clear();
    connection_upload_part_path.clear();
    connection_upload_last_activity_ms = 0;
}

[[maybe_unused]] void resetUploadSession(bool remove_partial)
{
    cleanupUploadSession(remove_partial);
    connection_pending_op = ConnectionUploadOp::None;
    connection_pending_done = false;
    connection_pending_ok = false;
    connection_pending_api_path.clear();
    connection_pending_full_path.clear();
    connection_pending_original_name.clear();
    connection_pending_error.clear();
    connection_pending_chunk.clear();
    connection_pending_total = 0;
    connection_pending_offset = 0;
}

bool queueUploadOp(ConnectionUploadOp op, const char* endpoint, uint32_t timeout_ms = 15000)
{
    if (!connection_upload_mutex) connection_upload_mutex = xSemaphoreCreateMutex();
    if (!connection_upload_complete) connection_upload_complete = xSemaphoreCreateBinary();
    if (!connection_upload_mutex || !connection_upload_complete) {
        connection_pending_error = "sync alloc";
        return false;
    }
    if (xSemaphoreTake(connection_upload_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        connection_pending_error = "busy";
        return false;
    }
    if (connection_pending_op != ConnectionUploadOp::None) {
        xSemaphoreGive(connection_upload_mutex);
        connection_pending_error = "busy";
        return false;
    }
    while (xSemaphoreTake(connection_upload_complete, 0) == pdTRUE) {}
    connection_pending_done = false;
    connection_pending_ok = false;
    connection_pending_error.clear();
    connection_pending_op = op;
    xSemaphoreGive(connection_upload_mutex);
    connection_dirty = true;
    dirty = true;
    if (xSemaphoreTake(connection_upload_complete, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        connection_pending_error = "timeout";
        setConnectionStatus(endpoint, "timeout");
        return false;
    }
    if (xSemaphoreTake(connection_upload_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        connection_pending_error = "result busy";
        return false;
    }
    const bool ok = connection_pending_ok;
    connection_pending_op = ConnectionUploadOp::None;
    xSemaphoreGive(connection_upload_mutex);
    setConnectionStatus(endpoint, ok ? "none" : connection_pending_error.c_str());
    return ok;
}

void processConnectionUploadOps()
{
    if (!connection_upload_mutex || xSemaphoreTake(connection_upload_mutex, 0) != pdTRUE) return;
    ConnectionUploadOp op = connection_pending_op;
    if (op == ConnectionUploadOp::None || connection_pending_done) {
        if (connection_upload_session && connection_upload_last_activity_ms &&
            M5.millis() - connection_upload_last_activity_ms > UPLOAD_SESSION_TIMEOUT_MS) {
            cleanupUploadSession(true);
            setConnectionStatus("upload-timeout", "aborted");
        }
        xSemaphoreGive(connection_upload_mutex);
        return;
    }
    bool ok = true;
    std::string err;

    if (op == ConnectionUploadOp::Begin) {
        cleanupUploadSession(true);
        char path_err[64] = {};
        std::string stored_api;
        std::string original_name;
        std::string parent_api;
        std::string final_path;
        if (!resolveUploadApiPath(connection_pending_api_path, &stored_api, &original_name, path_err, sizeof(path_err)) ||
            !prepareUploadPath(stored_api, &final_path, &parent_api, path_err, sizeof(path_err))) {
            ok = false;
            err = path_err[0] ? path_err : "bad path";
        } else {
            connection_pending_api_path = stored_api;
            connection_pending_full_path = final_path;
            connection_pending_original_name = original_name;
            struct stat target = {};
            if (stat(connection_pending_full_path.c_str(), &target) == 0) {
                ok = false;
                err = "exists";
            }
        }
        if (ok) {
            const size_t slash = connection_pending_full_path.find_last_of('/');
            connection_upload_part_path = connection_pending_full_path.substr(0, slash + 1) + "ABVXUP.TMP";
            unlink(connection_upload_part_path.c_str());
            FILE* f = fopen(connection_upload_part_path.c_str(), "wb");
            if (!f) {
                ok = false;
                err = "open part ";
                err += std::strerror(errno);
            } else {
                connection_upload_file = f;
                connection_upload_final_path = connection_pending_full_path;
                connection_upload_path = connection_pending_api_path;
                connection_upload_original_name = connection_pending_original_name;
                connection_upload_session = true;
                connection_upload_done = 0;
                connection_upload_total = static_cast<int>(connection_pending_total);
                connection_upload_active = true;
                connection_upload_last_activity_ms = M5.millis();
            }
        }
    } else if (op == ConnectionUploadOp::Chunk) {
        if (!connection_upload_session || !connection_upload_file || connection_upload_path != connection_pending_api_path) {
            ok = false;
            err = "no session";
        } else if (connection_pending_total != static_cast<size_t>(connection_upload_total)) {
            ok = false;
            err = "total mismatch";
        } else if (connection_pending_offset + connection_pending_chunk.size() == static_cast<size_t>(connection_upload_done)) {
            // Idempotent acknowledgement: the client may retry when the HTTP
            // response was lost after this exact chunk was already written.
            connection_upload_last_activity_ms = M5.millis();
        } else if (connection_pending_offset != static_cast<size_t>(connection_upload_done)) {
            ok = false;
            err = "offset mismatch";
        } else if (!connection_pending_chunk.empty()) {
            size_t wrote = fwrite(connection_pending_chunk.data(), 1, connection_pending_chunk.size(), connection_upload_file);
            if (wrote != connection_pending_chunk.size()) {
                ok = false;
                err = "write ";
                err += std::strerror(errno);
            } else {
                connection_upload_done += static_cast<int>(wrote);
                connection_upload_last_activity_ms = M5.millis();
                if (connection_upload_done % (64 * 1024) == 0 && fflush(connection_upload_file) != 0) {
                    ok = false;
                    err = "flush ";
                    err += std::strerror(errno);
                }
            }
        }
    } else if (op == ConnectionUploadOp::Finish) {
        if (!connection_upload_session || connection_upload_path != connection_pending_api_path ||
            static_cast<size_t>(connection_upload_done) != connection_pending_total) {
            ok = false;
            err = "size mismatch";
        }
        if (connection_upload_file) {
            if (fflush(connection_upload_file) != 0) {
                ok = false;
                err = "flush ";
                err += std::strerror(errno);
            }
            if (ok && fsync(fileno(connection_upload_file)) != 0) {
                ok = false;
                err = "sync ";
                err += std::strerror(errno);
            }
            if (fclose(connection_upload_file) != 0) {
                ok = false;
                err = "close ";
                err += std::strerror(errno);
            }
            connection_upload_file = nullptr;
        }
        if (ok) {
            struct stat st = {};
            if (stat(connection_upload_part_path.c_str(), &st) != 0 || static_cast<size_t>(st.st_size) != connection_pending_total) {
                ok = false;
                err = "size mismatch";
            }
        }
        if (ok && rename(connection_upload_part_path.c_str(), connection_upload_final_path.c_str()) != 0) {
            ok = false;
            err = "rename ";
            err += std::strerror(errno);
        }
        if (ok && !connection_upload_original_name.empty()) {
            appendMusicIndex(baseName(connection_upload_path), connection_upload_original_name);
        }
        if (ok) {
            connection_upload_active = false;
            connection_upload_done = static_cast<int>(connection_pending_total);
            connection_upload_total = static_cast<int>(connection_pending_total);
            connection_upload_session = false;
            connection_upload_path.clear();
            connection_upload_original_name.clear();
            connection_upload_final_path.clear();
            connection_upload_part_path.clear();
            connection_upload_last_activity_ms = 0;
        }
    } else if (op == ConnectionUploadOp::Abort) {
        cleanupUploadSession(true);
    }

    if (!ok && op != ConnectionUploadOp::Abort) {
        cleanupUploadSession(true);
    }
    connection_pending_chunk.clear();
    connection_pending_error = err;
    connection_pending_ok = ok;
    connection_pending_done = true;
    connection_dirty = true;
    dirty = true;
    xSemaphoreGive(connection_upload_mutex);
    xSemaphoreGive(connection_upload_complete);
}

bool ensureConnectionWriteDir(char* err, size_t err_len)
{
    if (!initSd()) {
        snprintf(err, err_len, "sd mount failed");
        return false;
    }
    struct stat st = {};
    if (stat(CONFIG_DIR, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return true;
        snprintf(err, err_len, "not dir");
        return false;
    }
    errno = 0;
    if (mkdir(CONFIG_DIR, 0775) != 0 && errno != EEXIST) {
        snprintf(err, err_len, "mkdir %d %s", errno, std::strerror(errno));
        return false;
    }
    return true;
}

esp_err_t connectionRootHandler(httpd_req_t* req)
{
    ++connection_req_count;
    setConnectionStatus("/", "none");
    httpd_resp_set_type(req, "text/html");
    const char* body =
        "<!doctype html><html><body>"
        "<h1>ABVx Connections v3</h1>"
        "<p>List/download and staged upload are available.</p>"
        "<ul>"
        "<li><a href=\"/api/ping\">/api/ping</a></li>"
        "<li><a href=\"/api/status\">/api/status</a></li>"
        "<li><a href=\"/api/list?path=/\">/api/list?path=/</a></li>"
        "<li><a href=\"/api/list?path=/music\">/api/list?path=/music</a></li>"
        "<li><a href=\"/api/list?path=/books\">/api/list?path=/books</a></li>"
        "<li><a href=\"/api/list?path=/notes\">/api/list?path=/notes</a></li>"
        "<li><a href=\"/api/list?path=/rec\">/api/list?path=/rec</a></li>"
        "</ul>"
        "<h2>Safe commands</h2>"
        "<pre>curl http://192.168.4.1/api/ping\n"
        "curl \"http://192.168.4.1/api/list?path=/music\"\n"
        "curl \"http://192.168.4.1/api/download?path=/notes/NOTE0001.TXT\"</pre>"
        "<p>Upload with tools/cardputer_upload.py. Files are written to ABVXUP.TMP and renamed only after verification.</p>"
        "<p>Use /cardputer as generic transfer folder. It appears as TRANSFER in Files.</p>"
        "<p>8.3 names are safest. No overwrite. Delete later.</p>"
        "</body></html>";
    return httpd_resp_sendstr(req, body);
}

esp_err_t connectionPingHandler(httpd_req_t* req)
{
    ++connection_req_count;
    setConnectionStatus("/api/ping", "none");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "OK PING\n");
}

bool getSignedQueryInt(httpd_req_t* req, const char* key, int* out)
{
    if (!out) return false;
    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len == 0 || query_len > 192) return false;
    char query[193] = {};
    char value[24] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, key, value, sizeof(value)) != ESP_OK) return false;
    char* end = nullptr;
    errno = 0;
    long parsed = std::strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed < -100000L || parsed > 100000L) return false;
    *out = static_cast<int>(parsed);
    return true;
}

bool getQueryEpoch(httpd_req_t* req, uint64_t* out)
{
    if (!out) return false;
    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len == 0 || query_len > 192) return false;
    char query[193] = {};
    char value[24] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "epoch", value, sizeof(value)) != ESP_OK) return false;
    char* end = nullptr;
    errno = 0;
    unsigned long long parsed = std::strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') return false;
    *out = static_cast<uint64_t>(parsed);
    return true;
}

esp_err_t connectionTimeSyncHandler(httpd_req_t* req)
{
    ++connection_req_count;
    const char* endpoint = "/api/time-sync";
    uint64_t epoch = 0;
    int offset_min = 0;
    // Accepted range: 2020-01-01 through 2100-01-01.
    if (!getQueryEpoch(req, &epoch) || !getSignedQueryInt(req, "offset", &offset_min) ||
        epoch < 1577836800ULL || epoch > 4102444800ULL || offset_min < -720 || offset_min > 840) {
        sendHttpError(req, endpoint, "bad epoch/offset");
        return ESP_OK;
    }
    portENTER_CRITICAL(&connection_time_mux);
    connection_time_pending_epoch = epoch;
    connection_time_pending_offset_min = offset_min;
    connection_time_sync_pending = true;
    portEXIT_CRITICAL(&connection_time_mux);
    setConnectionStatus(endpoint, "queued");
    connection_dirty = true;
    httpd_resp_set_type(req, "text/plain");
    char body[128];
    snprintf(body, sizeof(body), "OK TIME QUEUED\nepoch=%llu\noffset=%d\n",
             static_cast<unsigned long long>(epoch), offset_min);
    return httpd_resp_sendstr(req, body);
}

esp_err_t connectionStatusHandler(httpd_req_t* req)
{
    ++connection_req_count;
    setConnectionStatus("/api/status", "none");
    char upload_path[96] = "-";
    const char* upload_state = connection_upload_session ? "OPEN" : "IDLE";
    if (connection_upload_mutex && xSemaphoreTake(connection_upload_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (!connection_upload_path.empty()) snprintf(upload_path, sizeof(upload_path), "%s", connection_upload_path.c_str());
        if (connection_pending_op != ConnectionUploadOp::None) upload_state = "BUSY";
        xSemaphoreGive(connection_upload_mutex);
    }
    bool time_pending = false;
    bool time_applied = false;
    uint64_t time_epoch = 0;
    int time_offset = 0;
    portENTER_CRITICAL(&connection_time_mux);
    time_pending = connection_time_sync_pending;
    time_applied = connection_time_sync_applied;
    time_epoch = connection_time_last_epoch;
    time_offset = connection_time_last_offset_min;
    portEXIT_CRITICAL(&connection_time_mux);
    const char* time_state = time_pending ? "PENDING" : (time_applied ? "APPLIED" : "NONE");
    char body[640];
    snprintf(body, sizeof(body),
             "OK STATUS\nap=%s\nhttp=%s\nreq=%d\nlast=%s\nerr=%s\nupload=%s\npath=%s\ndone=%d\ntotal=%d\n"
             "time=%s\nepoch=%llu\noffset=%d\nclock=%lu\n"
             "cast=%s:%u\ncast_trace=%s|%s|%u|%ums|%d\ncast_retries=%lu\n",
             connection_wifi_on ? "ON" : "OFF",
             connection_http_on ? "ON" : "OFF",
             connection_req_count,
             connection_last_endpoint, connection_last_error,
             upload_state, upload_path, connection_upload_done, connection_upload_total,
             time_state, static_cast<unsigned long long>(time_epoch), time_offset,
             static_cast<unsigned long>(elapsedClockSeconds() % 86400),
             cast_host, static_cast<unsigned int>(cast_port), cast_last_endpoint,
             cast_last_path, cast_last_code, cast_last_latency_ms, cast_last_ok ? 1 : 0, static_cast<unsigned long>(cast_retries_total));
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, body);
}

esp_err_t connectionListHandler(httpd_req_t* req)
{
    ++connection_req_count;
    std::string api_path;
    if (!getRequestPath(req, &api_path, "/")) {
        sendHttpError(req, "/api/list", "missing path");
        return ESP_OK;
    }
    char err[48] = {};
    std::string full_path;
    if (!apiPathToSdPath(api_path, &full_path, err, sizeof(err))) {
        sendHttpError(req, "/api/list", err);
        return ESP_OK;
    }
    if (!initSd()) {
        sendHttpError(req, "/api/list", "no sd", HTTPD_500_INTERNAL_SERVER_ERROR);
        return ESP_OK;
    }
    DIR* dir = nullptr;
    if (api_path != "/") {
        dir = opendir(full_path.c_str());
        if (!dir) {
            snprintf(err, sizeof(err), "open %s", std::strerror(errno));
            sendHttpError(req, "/api/list", err, HTTPD_500_INTERNAL_SERVER_ERROR);
            return ESP_OK;
        }
    }

    setConnectionStatus("/api/list", "none");
    httpd_resp_set_type(req, "text/plain");
    char line[224];
    snprintf(line, sizeof(line), "OK LIST %s\n", api_path.c_str());
    httpd_resp_sendstr_chunk(req, line);

    if (api_path == "/") {
        static const char* roots[] = {"music", "books", "notes", "rec", "cardputer"};
        for (const char* root : roots) {
            snprintf(line, sizeof(line), "D 0 %s\n", root);
            httpd_resp_sendstr_chunk(req, line);
        }
        httpd_resp_sendstr_chunk(req, "END\n");
        httpd_resp_sendstr_chunk(req, nullptr);
        return ESP_OK;
    }

    while (dirent* entry = readdir(dir)) {
        std::string name = entry->d_name;
        if (isHidden(name)) continue;
        std::string child = full_path + "/" + name;
        struct stat st = {};
        if (stat(child.c_str(), &st) != 0) continue;
        bool dir_flag = S_ISDIR(st.st_mode) || entry->d_type == DT_DIR;
        snprintf(line, sizeof(line), "%c %lu %.96s\n", dir_flag ? 'D' : 'F', static_cast<unsigned long>(dir_flag ? 0 : st.st_size), name.c_str());
        httpd_resp_sendstr_chunk(req, line);
    }
    closedir(dir);
    httpd_resp_sendstr_chunk(req, "END\n");
    httpd_resp_sendstr_chunk(req, nullptr);
    return ESP_OK;
}

esp_err_t connectionDownloadHandler(httpd_req_t* req)
{
    ++connection_req_count;
    std::string api_path;
    if (!getRequestPath(req, &api_path, nullptr)) {
        sendHttpError(req, "/api/download", "missing path");
        return ESP_OK;
    }
    char err[64] = {};
    std::string full_path;
    if (!apiPathToSdPath(api_path, &full_path, err, sizeof(err)) || api_path == "/") {
        sendHttpError(req, "/api/download", err[0] ? err : "bad file");
        return ESP_OK;
    }
    if (!initSd()) {
        sendHttpError(req, "/api/download", "no sd", HTTPD_500_INTERNAL_SERVER_ERROR);
        return ESP_OK;
    }
    struct stat st = {};
    if (stat(full_path.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) {
        sendHttpError(req, "/api/download", "not file", HTTPD_404_NOT_FOUND);
        return ESP_OK;
    }
    FILE* f = fopen(full_path.c_str(), "rb");
    if (!f) {
        snprintf(err, sizeof(err), "open %s", std::strerror(errno));
        sendHttpError(req, "/api/download", err, HTTPD_500_INTERNAL_SERVER_ERROR);
        return ESP_OK;
    }

    setConnectionStatus("/api/download", "none");
    httpd_resp_set_type(req, "application/octet-stream");
    char disp[144];
    std::string download_name = baseName(api_path);
    for (char& c : download_name) {
        if (c == '"' || c == '\\' || static_cast<unsigned char>(c) < 32) c = '_';
    }
    snprintf(disp, sizeof(disp), "attachment; filename=\"%.96s\"", download_name.c_str());
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    uint8_t buf[1024];
    bool ok = true;
    while (true) {
        size_t n = fread(buf, 1, sizeof(buf), f);
        if (n > 0 && httpd_resp_send_chunk(req, reinterpret_cast<const char*>(buf), n) != ESP_OK) {
            ok = false;
            break;
        }
        if (n < sizeof(buf)) {
            if (ferror(f)) ok = false;
            break;
        }
        vTaskDelay(1);
    }
    fclose(f);
    if (ok) {
        httpd_resp_send_chunk(req, nullptr, 0);
    } else {
        setConnectionStatus("/api/download", "send/read failed");
    }
    return ESP_OK;
}

esp_err_t connectionWriteTestHandler(httpd_req_t* req)
{
    ++connection_req_count;
    sendHttpError(req, "/api/write-test", "disabled; use staged upload");
    return ESP_OK;
#if 0
    const char* endpoint = "/api/write-test";
    char err[64] = {};
    if (!ensureConnectionWriteDir(err, sizeof(err))) {
        sendHttpError(req, endpoint, err[0] ? err : "sd dir failed", HTTPD_500_INTERNAL_SERVER_ERROR);
        return ESP_OK;
    }

    const char* path = "/sdcard/CARDPTR/WTEST.TXT";
    FILE* f = fopen(path, "wb");
    if (!f) {
        snprintf(err, sizeof(err), "open %s", std::strerror(errno));
        sendHttpError(req, endpoint, err, HTTPD_500_INTERNAL_SERVER_ERROR);
        return ESP_OK;
    }

    const char* body = "write test\n";
    size_t wanted = std::strlen(body);
    size_t n = fwrite(body, 1, wanted, f);
    bool ok = n == wanted;
    if (fflush(f) != 0) ok = false;
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        sendHttpError(req, endpoint, "write failed", HTTPD_500_INTERNAL_SERVER_ERROR);
        return ESP_OK;
    }

    setConnectionStatus(endpoint, "none");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "OK WRITE\npath=/cardputer/WTEST.TXT\n");
#endif
}

esp_err_t connectionUploadHandler(httpd_req_t* req)
{
    ++connection_req_count;
    const char* endpoint = "/api/upload";
    sendHttpError(req, endpoint, "use upload-begin/chunk/finish");
    return ESP_OK;
#if 0
    std::string api_path;
    if (!getRequestPath(req, &api_path, nullptr)) {
        sendHttpError(req, endpoint, "missing path");
        return ESP_OK;
    }
    if (req->content_len <= 0 || static_cast<size_t>(req->content_len) > MAX_UPLOAD_BYTES) {
        sendHttpError(req, endpoint, "bad size");
        return ESP_OK;
    }
    if (static_cast<size_t>(req->content_len) > MAX_DIRECT_UPLOAD_BYTES) {
        sendHttpError(req, endpoint, "use chunk upload");
        return ESP_OK;
    }

    char err[64] = {};
    std::string stored_api, original_name;
    if (!resolveUploadApiPath(api_path, &stored_api, &original_name, err, sizeof(err))) {
        sendHttpError(req, endpoint, err[0] ? err : "bad path");
        return ESP_OK;
    }
    std::string parent_api, full_path;
    if (!prepareUploadPath(stored_api, &full_path, &parent_api, err, sizeof(err))) {
        sendHttpError(req, endpoint, err[0] ? err : "bad path", HTTPD_500_INTERNAL_SERVER_ERROR);
        return ESP_OK;
    }

    struct stat st = {};
    if (stat(full_path.c_str(), &st) == 0) {
        sendHttpError(req, endpoint, "exists");
        return ESP_OK;
    }

    FILE* f = fopen(full_path.c_str(), "wb");
    if (!f) {
        snprintf(err, sizeof(err), "open %s", std::strerror(errno));
        sendHttpError(req, endpoint, err, HTTPD_500_INTERNAL_SERVER_ERROR);
        return ESP_OK;
    }

    char buf[1024];
    int remaining = req->content_len;
    connection_upload_active = true;
    connection_upload_done = 0;
    connection_upload_total = req->content_len;
    setConnectionStatus(endpoint, "uploading");
    bool ok = true;
    int timeout_count = 0;
    const uint32_t receive_started_ms = M5.millis();
    while (remaining > 0) {
        int want = std::min<int>(remaining, sizeof(buf));
        int got = httpd_req_recv(req, buf, want);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) {
            ++timeout_count;
            if (timeout_count <= 3 && M5.millis() - receive_started_ms < 15000) continue;
            ok = false;
            snprintf(err, sizeof(err), "recv timeout");
            break;
        }
        if (got <= 0) {
            ok = false;
            snprintf(err, sizeof(err), "recv failed");
            break;
        }
        if (fwrite(buf, 1, got, f) != static_cast<size_t>(got)) {
            ok = false;
            snprintf(err, sizeof(err), "write %s", std::strerror(errno));
            break;
        }
        remaining -= got;
        timeout_count = 0;
        connection_upload_done = req->content_len - remaining;
        connection_dirty = true;
        vTaskDelay(1);
    }
    if (fflush(f) != 0) {
        ok = false;
        snprintf(err, sizeof(err), "flush %s", std::strerror(errno));
    }
    if (fclose(f) != 0) {
        ok = false;
        snprintf(err, sizeof(err), "close %s", std::strerror(errno));
    }
    if (!ok) {
        connection_upload_active = false;
        unlink(full_path.c_str());
        sendHttpError(req, endpoint, err[0] ? err : "upload failed", HTTPD_500_INTERNAL_SERVER_ERROR);
        return ESP_OK;
    }

    connection_upload_active = false;
    if (!original_name.empty()) appendMusicIndex(baseName(stored_api), original_name);
    setConnectionStatus(endpoint, "none");
    httpd_resp_set_type(req, "text/plain");
    char reply[192];
    snprintf(reply, sizeof(reply), "OK UPLOAD\npath=%s\nstored=%s\nsize=%d\n", api_path.c_str(), stored_api.c_str(), req->content_len);
    return httpd_resp_sendstr(req, reply);
#endif
}

esp_err_t connectionUploadBeginHandler(httpd_req_t* req)
{
    ++connection_req_count;
    const char* endpoint = "/api/upload-begin";
    std::string api_path;
    size_t total = 0;
    if (!getRequestPath(req, &api_path, nullptr) || !getQueryUint(req, "size", &total)) {
        sendHttpError(req, endpoint, "missing args");
        return ESP_OK;
    }
    if (total == 0 || total > MAX_UPLOAD_BYTES) {
        sendHttpError(req, endpoint, "bad size");
        return ESP_OK;
    }
    connection_pending_api_path = api_path;
    connection_pending_full_path.clear();
    connection_pending_original_name.clear();
    connection_pending_total = total;
    if (!queueUploadOp(ConnectionUploadOp::Begin, endpoint)) {
        sendHttpError(req, endpoint, connection_pending_error.empty() ? "begin failed" : connection_pending_error.c_str(), HTTPD_500_INTERNAL_SERVER_ERROR);
        return ESP_OK;
    }
    setConnectionStatus(endpoint, "begin");
    httpd_resp_set_type(req, "text/plain");
    char reply[192];
    snprintf(reply, sizeof(reply), "OK BEGIN\npath=%s\nstored=%s\n", api_path.c_str(), connection_pending_api_path.c_str());
    return httpd_resp_sendstr(req, reply);
}

esp_err_t connectionUploadChunkHandler(httpd_req_t* req)
{
    ++connection_req_count;
    const char* endpoint = "/api/upload-chunk";
    std::string api_path;
    size_t offset = 0, total = 0;
    if (!getRequestPath(req, &api_path, nullptr) || !getQueryUint(req, "offset", &offset) || !getQueryUint(req, "total", &total)) {
        sendHttpError(req, endpoint, "missing args");
        return ESP_OK;
    }
    if (req->content_len <= 0 || static_cast<size_t>(req->content_len) > MAX_UPLOAD_CHUNK_BYTES ||
        total == 0 || total > MAX_UPLOAD_BYTES || offset + static_cast<size_t>(req->content_len) > total) {
        sendHttpError(req, endpoint, "bad chunk");
        return ESP_OK;
    }
    char err[64] = {};
    std::vector<uint8_t> recv_buf(req->content_len);
    int remaining = req->content_len;
    connection_upload_active = true;
    connection_upload_done = static_cast<int>(offset);
    connection_upload_total = static_cast<int>(total);
    setConnectionStatus(endpoint, "chunk recv");
    bool ok = true;
    int pos = 0;
    while (remaining > 0) {
        int got = httpd_req_recv(req, reinterpret_cast<char*>(recv_buf.data() + pos), remaining);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0) {
            ok = false;
            snprintf(err, sizeof(err), "recv failed");
            break;
        }
        pos += got;
        remaining -= got;
        connection_dirty = true;
        vTaskDelay(2);
    }
    if (!ok) {
        queueUploadOp(ConnectionUploadOp::Abort, endpoint, 5000);
        sendHttpError(req, endpoint, err[0] ? err : "chunk failed", HTTPD_500_INTERNAL_SERVER_ERROR);
        return ESP_OK;
    }
    connection_pending_api_path = api_path;
    connection_pending_offset = offset;
    connection_pending_total = total;
    connection_pending_chunk.swap(recv_buf);
    if (!queueUploadOp(ConnectionUploadOp::Chunk, endpoint)) {
        sendHttpError(req, endpoint, connection_pending_error.empty() ? "chunk failed" : connection_pending_error.c_str(), HTTPD_500_INTERNAL_SERVER_ERROR);
        return ESP_OK;
    }
    connection_upload_active = false;
    setConnectionStatus(endpoint, "chunk");
    httpd_resp_set_type(req, "text/plain");
    char reply[96];
    snprintf(reply, sizeof(reply), "OK CHUNK\ndone=%d\ntotal=%d\n", connection_upload_done, connection_upload_total);
    return httpd_resp_sendstr(req, reply);
}

esp_err_t connectionUploadFinishHandler(httpd_req_t* req)
{
    ++connection_req_count;
    const char* endpoint = "/api/upload-finish";
    std::string api_path;
    size_t total = 0;
    if (!getRequestPath(req, &api_path, nullptr) || !getQueryUint(req, "size", &total)) {
        sendHttpError(req, endpoint, "missing args");
        return ESP_OK;
    }
    connection_pending_api_path = api_path;
    connection_pending_total = total;
    if (!queueUploadOp(ConnectionUploadOp::Finish, endpoint)) {
        sendHttpError(req, endpoint, connection_pending_error.empty() ? "finish failed" : connection_pending_error.c_str(), HTTPD_500_INTERNAL_SERVER_ERROR);
        return ESP_OK;
    }
    setConnectionStatus(endpoint, "none");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "OK FINISH\n");
}

esp_err_t connectionUploadAbortHandler(httpd_req_t* req)
{
    ++connection_req_count;
    const char* endpoint = "/api/upload-abort";
    std::string api_path;
    if (!getRequestPath(req, &api_path, nullptr)) {
        sendHttpError(req, endpoint, "missing path");
        return ESP_OK;
    }
    connection_pending_api_path = api_path;
    if (!queueUploadOp(ConnectionUploadOp::Abort, endpoint, 5000)) {
        sendHttpError(req, endpoint, connection_pending_error.empty() ? "abort failed" : connection_pending_error.c_str(), HTTPD_500_INTERNAL_SERVER_ERROR);
        return ESP_OK;
    }
    setConnectionStatus(endpoint, "aborted");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "OK ABORT\n");
}

bool ensureConnectionStack(char* err, size_t err_len)
{
    if (connection_stack_ready) return true;

    esp_err_t rc = nvs_flash_init();
    if (rc == ESP_ERR_NVS_NO_FREE_PAGES || rc == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        rc = nvs_flash_init();
    }
    if (rc != ESP_OK) {
        snprintf(err, err_len, "nvs %s", esp_err_to_name(rc));
        return false;
    }

    rc = esp_netif_init();
    if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) {
        snprintf(err, err_len, "netif %s", esp_err_to_name(rc));
        return false;
    }
    rc = esp_event_loop_create_default();
    if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) {
        snprintf(err, err_len, "event %s", esp_err_to_name(rc));
        return false;
    }

    connection_ap_netif = esp_netif_create_default_wifi_ap();
    if (!connection_ap_netif) {
        snprintf(err, err_len, "ap netif");
        return false;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    rc = esp_wifi_init(&cfg);
    if (rc != ESP_OK) {
        snprintf(err, err_len, "wifi init %s", esp_err_to_name(rc));
        return false;
    }
    connection_stack_ready = true;
    return true;
}

bool startConnectionHttp(char* err, size_t err_len)
{
    if (connection_httpd) return true;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.lru_purge_enable = true;
    // Default ESP-IDF HTTP server handler limit is too small for cast endpoints
    // plus staged upload handlers.
    config.max_uri_handlers = 24;
    esp_err_t rc = httpd_start(&connection_httpd, &config);
    if (rc != ESP_OK) {
        snprintf(err, err_len, "http %s", esp_err_to_name(rc));
        connection_httpd = nullptr;
        return false;
    }

    auto reg = [&](const char* uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t*)) -> bool {
        httpd_uri_t route = {};
        route.uri = uri;
        route.method = method;
        route.handler = handler;
        esp_err_t reg_rc = httpd_register_uri_handler(connection_httpd, &route);
        if (reg_rc != ESP_OK) {
            snprintf(err, err_len, "route %s %s", uri, esp_err_to_name(reg_rc));
            httpd_stop(connection_httpd);
            connection_httpd = nullptr;
            connection_http_on = false;
            return false;
        }
        return true;
    };

    if (!reg("/", HTTP_GET, connectionRootHandler)) return false;
    if (!reg("/api/ping", HTTP_GET, connectionPingHandler)) return false;
    if (!reg("/api/status", HTTP_GET, connectionStatusHandler)) return false;
    if (!reg("/api/time-sync", HTTP_POST, connectionTimeSyncHandler)) return false;
    if (!reg("/api/list", HTTP_GET, connectionListHandler)) return false;
    if (!reg("/api/download", HTTP_GET, connectionDownloadHandler)) return false;
    if (!reg("/api/write-test", HTTP_GET, connectionWriteTestHandler)) return false;
    if (!reg("/api/write-test", HTTP_POST, connectionWriteTestHandler)) return false;
    if (!reg("/api/upload", HTTP_POST, connectionUploadHandler)) return false;
    if (!reg("/api/upload-begin", HTTP_POST, connectionUploadBeginHandler)) return false;
    if (!reg("/api/upload-chunk", HTTP_POST, connectionUploadChunkHandler)) return false;
    if (!reg("/api/upload-finish", HTTP_POST, connectionUploadFinishHandler)) return false;
    if (!reg("/api/upload-abort", HTTP_POST, connectionUploadAbortHandler)) return false;
    if (!reg("/api/cast/status", HTTP_GET, connectionCastStatusHandler)) return false;
    if (!reg("/api/cast/play", HTTP_POST, connectionCastPlayHandler)) return false;
    if (!reg("/api/cast/next", HTTP_POST, connectionCastNextHandler)) return false;
    if (!reg("/api/cast/prev", HTTP_POST, connectionCastPrevHandler)) return false;
    connection_http_on = true;
    return true;
}

bool startConnections(char* err, size_t err_len)
{
    if (connection_wifi_on && connection_http_on) return true;
    // Mount SD before Wi-Fi starts. SD operations from HTTP handlers are then
    // less likely to be the first mount attempt from the HTTP server task.
    initSd();
    if (!ensureConnectionStack(err, err_len)) return false;
    if (!connection_upload_mutex) connection_upload_mutex = xSemaphoreCreateMutex();
    if (!connection_upload_complete) connection_upload_complete = xSemaphoreCreateBinary();
    if (!connection_upload_mutex || !connection_upload_complete) {
        snprintf(err, err_len, "upload sync alloc");
        return false;
    }

    wifi_config_t ap_config = {};
    const char* ssid = "ABVX-Cardputer";
    snprintf(connection_ap_password, sizeof(connection_ap_password), "cardputer");
    const char* pass = connection_ap_password;
    snprintf(reinterpret_cast<char*>(ap_config.ap.ssid), sizeof(ap_config.ap.ssid), "%s", ssid);
    snprintf(reinterpret_cast<char*>(ap_config.ap.password), sizeof(ap_config.ap.password), "%s", pass);
    ap_config.ap.ssid_len = std::strlen(ssid);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 1;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

    esp_err_t rc = esp_wifi_set_mode(WIFI_MODE_AP);
    if (rc != ESP_OK) {
        snprintf(err, err_len, "mode %s", esp_err_to_name(rc));
        return false;
    }
    rc = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (rc != ESP_OK) {
        snprintf(err, err_len, "config %s", esp_err_to_name(rc));
        return false;
    }
    rc = esp_wifi_start();
    if (rc != ESP_OK) {
        snprintf(err, err_len, "start %s", esp_err_to_name(rc));
        return false;
    }
    connection_wifi_on = true;
    if (!startConnectionHttp(err, err_len)) return false;
    setConnectionStatus("started", "none");
    return true;
}

void stopConnections()
{
    if (connection_upload_mutex && xSemaphoreTake(connection_upload_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        cleanupUploadSession(true);
        if (connection_pending_op != ConnectionUploadOp::None && !connection_pending_done) {
            connection_pending_error = "connections stopped";
            connection_pending_ok = false;
            connection_pending_done = true;
            xSemaphoreGive(connection_upload_complete);
        }
        xSemaphoreGive(connection_upload_mutex);
    }
    if (connection_httpd) {
        httpd_stop(connection_httpd);
        connection_httpd = nullptr;
    }
    connection_http_on = false;
    if (connection_wifi_on) {
        esp_wifi_stop();
    }
    connection_wifi_on = false;
    setConnectionStatus("stopped", "none");
}

void drawConnections()
{
    canvas.fillScreen(uiBg());
    drawCyberAccent();
    canvas.setTextSize(2);
    canvas.setTextColor(uiFg(), uiBg());
    canvas.setCursor(8, 8);
    if (connection_wifi_on || connection_http_on) {
        canvas.print("TRANSFER");
        canvas.setTextColor(uiAccent(), uiBg());
        canvas.setCursor(8, 32);
        canvas.printf("AP %s", connection_wifi_on ? "ON" : "OFF");
        canvas.setCursor(104, 32);
        canvas.printf("HTTP %s", connection_http_on ? "ON" : "OFF");
        canvas.setTextSize(1);
        canvas.setTextColor(uiFg(), uiBg());
        canvas.setCursor(8, 58);
        canvas.print("SSID ABVX-Cardputer");
        canvas.setCursor(8, 70);
        canvas.printf("PASS %s", connection_ap_password);
        canvas.setCursor(8, 82);
        canvas.print("URL  192.168.4.1");
        canvas.setCursor(8, 94);
        canvas.printf("CAST %s:%u", cast_host, static_cast<unsigned int>(cast_port));
        canvas.setCursor(8, 104);
        canvas.setTextColor(uiAccent(), uiBg());
        canvas.printf("%s %u %s", cast_last_ok ? "OK" : "FAIL", cast_last_code, cast_last_error);
        canvas.setTextColor(uiAccent(), uiBg());
        canvas.setCursor(8, 114);
        canvas.printf("LAST %s", cast_last_path);
        canvas.setTextColor(uiDim(), uiBg());
        canvas.setCursor(8, 122);
        if (connection_upload_active) {
            canvas.printf("UPLOAD %d/%d", connection_upload_done, connection_upload_total);
        } else {
            canvas.print("STAGED UPLOAD READY");
        }
        canvas.setTextColor(uiDim(), uiBg());
        canvas.setCursor(8, 126);
        canvas.print("OK STATUS            GO STOP");
        canvas.pushSprite(0, 0);
        return;
    }
    canvas.print("TRANSFER");
    canvas.setTextColor(uiAccent(), uiBg());
    canvas.setCursor(8, 36);
    canvas.print("AP OFF");
    canvas.setTextColor(uiFg(), uiBg());
    canvas.setCursor(8, 62);
    canvas.print("OK START");
    canvas.setCursor(8, 84);
    canvas.print("List/download files");
        canvas.setTextSize(1);
        canvas.setTextColor(uiDim(), uiBg());
        canvas.setCursor(8, 106);
        canvas.print("Staged upload v3");
        canvas.setCursor(8, 112);
        canvas.print("CAST API READY");
        canvas.setCursor(8, 124);
        canvas.print("OK START         GO BACK");
        canvas.pushSprite(0, 0);
}

void drawMessage()
{
    canvas.fillScreen(uiBg());
    drawCyberAccent();
    canvas.setTextSize(2);
    canvas.setTextColor(uiFg(), uiBg());
    canvas.setCursor(8, 10);
    canvas.println(message_title.c_str());
    canvas.setTextSize(2);
    int y = 42;
    size_t pos = 0;
    while (pos <= message_body.size() && y <= 94) {
        size_t nl = message_body.find('\n', pos);
        std::string line = message_body.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        canvas.setCursor(8, y);
        canvas.printf("%.18s", line.c_str());
        if (nl == std::string::npos) break;
        pos = nl + 1;
        y += 24;
    }
    canvas.setTextSize(1);
    canvas.setTextColor(uiDim(), uiBg());
    canvas.setCursor(8, 122);
    canvas.print("OK/GO BACK");
    canvas.pushSprite(0, 0);
}

void drawIfDirty()
{
    if (!dirty || display_off) return;
    if (screen == Screen::Launcher) drawLauncher();
    else if (screen == Screen::Dashboard) drawDashboard();
    else if (screen == Screen::MusicList) drawMusicList();
    else if (screen == Screen::MusicInfo) drawMusicInfo();
    else if (screen == Screen::MusicPlaying) drawMusicPlaying();
    else if (screen == Screen::ReaderList) drawReaderList();
    else if (screen == Screen::ReaderView) drawReaderView();
    else if (screen == Screen::ReaderSpeed) drawReaderSpeed();
    else if (screen == Screen::NotesList) drawNotesList();
    else if (screen == Screen::NotesView) drawNotesView();
    else if (screen == Screen::NotesEdit) drawNotesEdit();
    else if (screen == Screen::NotesDeleteConfirm) drawNotesDeleteConfirm();
    else if (screen == Screen::RecorderList) drawRecorderList();
    else if (screen == Screen::RecorderRecording) drawRecorderRecording();
    else if (screen == Screen::RecorderPlaying) drawRecorderPlaying();
    else if (screen == Screen::RecorderDeleteConfirm) drawRecorderDeleteConfirm();
    else if (screen == Screen::TimeApp) drawTimeApp();
    else if (screen == Screen::FilesList) drawFilesList();
    else if (screen == Screen::FilesInfo) drawFilesInfo();
    else if (screen == Screen::FilesDeleteConfirm) drawFilesDeleteConfirm();
    else if (screen == Screen::Randomizer) drawRandomizer();
    else if (screen == Screen::HabitsList) drawHabitsList();
    else if (screen == Screen::HabitsStats) drawHabitsStats();
    else if (screen == Screen::HabitsManage) drawHabitsManage();
    else if (screen == Screen::HabitsEdit) drawHabitsEdit();
    else if (screen == Screen::HabitsDisableConfirm) drawHabitsDisableConfirm();
    else if (screen == Screen::Settings) drawSettings();
    else if (screen == Screen::Connections) drawConnections();
    else if (screen == Screen::InboxList) drawInboxList();
    else if (screen == Screen::InboxDetail) drawInboxDetail();
    else drawMessage();
    dirty = false;
}

void updateSpeedReader()
{
    if (screen != Screen::ReaderSpeed || speed_paused) return;
    uint32_t now = M5.millis();
    if (now < speed_next_ms) return;
    if (reader_streaming) {
        if (reader_stream_speed_next_offset == 0 || reader_stream_speed_next_offset >= reader_stream_file_size) {
            speed_paused = true;
            dirty = true;
            return;
        }
        reader_stream_speed_offset = reader_stream_speed_next_offset;
        std::string ignored;
        if (!loadReaderStreamSpeedUnit(&ignored)) {
            speed_paused = true;
            reader_stream_status = "SPEED: " + (ignored.empty() ? std::string("read failed") : ignored);
        }
        saveReaderBookmark();
        speed_next_ms = now + speedIntervalMs();
        if (!display_off) dirty = true;
        return;
    }
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
    uint32_t play_dim = 10000;
    uint32_t play_off = 30000;
    uint32_t idle_off = 60000;
    if (timeout_mode == TimeoutMode::Short) {
        play_dim = 5000;
        play_off = 15000;
        idle_off = 30000;
    } else if (timeout_mode == TimeoutMode::Long) {
        play_dim = 30000;
        play_off = 90000;
        idle_off = 180000;
    }
    if (playing) {
        if (idle > play_off && !display_off) {
            M5.Display.setBrightness(0);
            display_off = true;
            display_dim = false;
        } else if (idle > play_dim && !display_dim && !display_off) {
            M5.Display.setBrightness(15);
            display_dim = true;
        }
    } else if (idle > idle_off && !display_off) {
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

char shortcutChar(KeyEvent ev)
{
    if (!ev.name || !ev.name[0] || ev.name[1]) return 0;
    unsigned char c = static_cast<unsigned char>(ev.name[0]);
    if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
    return static_cast<char>(c);
}

bool isMusicShuffleKey(KeyEvent ev)
{
    return ev.key == Key::One || shortcutChar(ev) == 's';
}

bool isMusicInfoKey(KeyEvent ev)
{
    return ev.key == Key::Two || shortcutChar(ev) == 'i';
}

bool handleOneButtonCapture(KeyEvent ev)
{
    if (screen != Screen::Launcher) return false;
    char c = shortcutChar(ev);
    if (!c) return false;
    if (c == 'r') {
        last_resume_target = ResumeTarget::Recorder;
        std::string err;
        if (!startRecording(&err)) {
            showMessage("Record failed", err.empty() ? "start" : err);
        }
        blockInput(350);
        dirty = true;
        return true;
    }
    if (c == 'n') {
        last_resume_target = ResumeTarget::Notes;
        note_input.clear();
        note_edit_existing = false;
        screen = Screen::NotesEdit;
        blockInput(300);
        dirty = true;
        return true;
    }
    if (c == 'm') {
        last_resume_target = ResumeTarget::Music;
        scanMusic();
        screen = Screen::MusicList;
        music_autostart_pending = true;
        blockInput(350);
        dirty = true;
        return true;
    }
    if (c == 'd' || c == '0') {
        openDashboard();
        return true;
    }
    if (c == '2' || c == '@' || c == 's') {
        resumeContext();
        return true;
    }
    return false;
}

void processMusicAutostart()
{
    if (!music_autostart_pending) return;
    if (M5.millis() < input_block_until_ms) return;
    music_autostart_pending = false;
    if (screen != Screen::MusicList && screen != Screen::Launcher) return;
    std::string err;
    const std::string path = selectedPath();
    if (!startPlayback(&err)) {
        showMessage("BAD MP3", path.empty() ? (err.empty() ? "no music" : err) : musicProblemBody(path, err));
    }
    dirty = true;
}

bool openInboxEvent(std::string* err = nullptr)
{
    if (inbox_entries.empty() || inbox_cursor < 0 || inbox_cursor >= static_cast<int>(inbox_entries.size())) {
        if (err) *err = "no event";
        return false;
    }
    const InboxEventView event = parseInboxEvent(inbox_entries[inbox_cursor]);

    if (event.type == "READ") {
        scanBooks();
        auto it = std::find(books.begin(), books.end(), event.detail);
        if (it == books.end()) {
            if (err) *err = "book missing";
            return false;
        }
        selected_book = static_cast<int>(std::distance(books.begin(), it));
        if (!loadSelectedBook(err)) return false;
        last_resume_target = ResumeTarget::Reader;
        screen = Screen::ReaderView;
        return true;
    }
    if (event.type == "LISTEN") {
        scanMusic();
        auto it = std::find(tracks.begin(), tracks.end(), event.detail);
        if (it == tracks.end()) {
            if (err) *err = "track missing";
            return false;
        }
        selected_track = static_cast<int>(std::distance(tracks.begin(), it));
        override_music_path.clear();
        if (!startPlayback(err)) return false;
        last_resume_target = ResumeTarget::Music;
        return true;
    }
    if (event.type == "NOTE" || event.type == "NOTE EDIT") {
        scanNotes();
        auto it = std::find(notes.begin(), notes.end(), event.detail);
        if (it == notes.end()) {
            if (err) *err = "note missing";
            return false;
        }
        notes_cursor = static_cast<int>(std::distance(notes.begin(), it)) + 1;
        if (!loadSelectedNote(err)) return false;
        last_note_name = active_note_name;
        last_resume_target = ResumeTarget::Notes;
        screen = Screen::NotesView;
        return true;
    }
    if (event.type == "VOICE") {
        scanRecordings();
        auto it = std::find(recordings.begin(), recordings.end(), event.detail);
        if (it == recordings.end()) {
            if (err) *err = "voice missing";
            return false;
        }
        recorder_cursor = static_cast<int>(std::distance(recordings.begin(), it)) + 1;
        if (!startRecordingPlayback(err)) return false;
        last_resume_target = ResumeTarget::Recorder;
        return true;
    }
    if (event.type == "HABIT") {
        last_resume_target = ResumeTarget::Habits;
        screen = Screen::HabitsList;
        return true;
    }
    if (event.type == "TIMER") {
        last_resume_target = ResumeTarget::Time;
        time_mode = TimeMode::Timer;
        screen = Screen::TimeApp;
        return true;
    }
    if (err) *err = "unsupported event";
    return false;
}

void handleKey(KeyEvent ev)
{
    const bool has_shortcut_char = shortcutChar(ev) != 0;
    if (ev.key == Key::None && !has_shortcut_char) return;
    if (ev.key == Key::None &&
        screen != Screen::NotesEdit &&
        screen != Screen::HabitsEdit &&
        screen != Screen::HabitsList &&
        screen != Screen::Launcher &&
        screen != Screen::Dashboard &&
        screen != Screen::MusicList &&
        screen != Screen::MusicInfo &&
        screen != Screen::MusicPlaying) return;
    last_input_ms = M5.millis();
    if (display_off || display_dim) {
        wakeDisplay();
        return;
    }
    if (handleOneButtonCapture(ev)) return;

    if (screen == Screen::Launcher) {
        if (ev.key == Key::Up) { launcher_index = (launcher_index + 10) % 11; pulseUi(); }
        else if (ev.key == Key::Down) { launcher_index = (launcher_index + 1) % 11; pulseUi(); }
        else if (ev.key == Key::Home) { launcher_index = 0; scanMusic(); screen = Screen::MusicList; pulseUi(); }
        else if (ev.key == Key::Two) resumeContext();
        else if (ev.key == Key::Ok) openLauncherApp(launcher_index);
        dirty = true;
        return;
    }

    if (screen == Screen::Dashboard) {
        if (ev.key == Key::Ok) resumeContext();
        else if (shortcutChar(ev) == '1') { last_resume_target = ResumeTarget::Music; resumeContext(); }
        else if (shortcutChar(ev) == '2') { last_resume_target = ResumeTarget::Reader; resumeContext(); }
        else if (shortcutChar(ev) == '3') { last_resume_target = ResumeTarget::Notes; resumeContext(); }
        else if (ev.key == Key::Home || ev.key == Key::Back) {
            screen = Screen::Launcher;
            blockInput(250);
        }
        dirty = true;
        return;
    }

    if (screen == Screen::InboxList) {
        if (ev.key == Key::Up && !inbox_entries.empty()) inbox_cursor = std::max(0, inbox_cursor - 1);
        else if (ev.key == Key::Down && !inbox_entries.empty()) inbox_cursor = std::min(static_cast<int>(inbox_entries.size()) - 1, inbox_cursor + 1);
        else if (ev.key == Key::Ok && !inbox_entries.empty()) {
            screen = Screen::InboxDetail;
            blockInput(200);
        }
        else if (ev.key == Key::One) refreshInboxManual();
        else if (ev.key == Key::Home || ev.key == Key::Back) {
            screen = Screen::Launcher;
            blockInput(250);
        }
        dirty = true;
        return;
    }

    if (screen == Screen::InboxDetail) {
        if (ev.key == Key::One) {
            refreshInboxManual();
            screen = Screen::InboxList;
            blockInput(200);
        } else if (ev.key == Key::Ok) {
            std::string err;
            if (!openInboxEvent(&err)) {
                inbox_status = "OPEN ERR " + inboxSafeText(err);
                screen = Screen::InboxList;
            }
            blockInput(350);
        } else if (ev.key == Key::Home || ev.key == Key::Back) {
            screen = Screen::InboxList;
            blockInput(200);
        }
        dirty = true;
        return;
    }

    if (screen == Screen::MusicList) {
        if (ev.key == Key::Up && !tracks.empty()) {
            selected_track = (selected_track - 1 + static_cast<int>(tracks.size())) % static_cast<int>(tracks.size());
        }
        else if (ev.key == Key::Down && !tracks.empty()) {
            selected_track = (selected_track + 1) % static_cast<int>(tracks.size());
        }
        else if (ev.key == Key::Left) nextTrack(-1);
        else if (ev.key == Key::Right) nextTrack(1);
        else if (isMusicShuffleKey(ev)) setShuffleEnabled(!shuffle_on);
        else if (isMusicInfoKey(ev)) {
            if (!tracks.empty()) {
                prepareMusicInfo();
                screen = Screen::MusicInfo;
            }
            blockInput(250);
        }
        else if (ev.key == Key::Ok) {
            if (!tracks.empty()) {
                override_music_path.clear();
                std::string err;
                const std::string path = selectedPath();
                if (!startPlayback(&err)) {
                    showMessage("BAD MP3", musicProblemBody(path, err), MessageReturn::Music);
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

    if (screen == Screen::MusicInfo) {
        if (ev.key == Key::Ok) {
            if (!tracks.empty()) {
                override_music_path.clear();
                std::string err;
                const std::string path = selectedPath();
                if (!startPlayback(&err)) {
                    showMessage("BAD MP3", musicProblemBody(path, err), MessageReturn::Music);
                    blockInput(500);
                }
            }
        } else if (isMusicInfoKey(ev) || shortcutChar(ev) == 'p') {
            std::string status;
            probeSelectedMusic(&status);
            music_info_status = status.empty() ? "UNKNOWN" : status;
            blockInput(300);
        } else if (ev.key == Key::Home || ev.key == Key::Back) {
            screen = Screen::MusicList;
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
        else if (isMusicShuffleKey(ev)) setShuffleEnabled(!shuffle_on);
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
                    showMessage("Read failed", err.empty() ? "open" : err);
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
        if (reader_streaming) {
            reader_stream_status.clear();
            if (ev.key == Key::Up) readerStreamMoveBack(1);
            else if (ev.key == Key::Down) readerStreamMoveForward(1);
            else if (ev.key == Key::Left) readerStreamMoveBack(READER_LINES_PER_PAGE);
            else if (ev.key == Key::Right) readerStreamMoveForward(READER_LINES_PER_PAGE);
            else if (ev.key == Key::One) {
                speed_mode = SpeedMode::OneWord;
                speed_paused = true;
                reader_stream_speed_offset = reader_stream_offset;
                std::string speed_error;
                if (loadReaderStreamSpeedUnit(&speed_error)) {
                    speed_next_ms = M5.millis() + speedIntervalMs();
                    screen = Screen::ReaderSpeed;
                    blockInput(300);
                } else {
                    reader_stream_status = "SPEED: " + (speed_error.empty() ? std::string("read failed") : speed_error);
                }
            }
            else if (ev.key == Key::Home || ev.key == Key::Back) {
                saveReaderBookmark();
                saveReaderState();
                closeReaderStream();
                screen = Screen::ReaderList;
                blockInput(250);
            }
            if (screen == Screen::ReaderView) saveReaderBookmark();
            dirty = true;
            return;
        }
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
            saveReaderState();
            screen = Screen::ReaderList;
            blockInput(250);
        }
        dirty = true;
        return;
    }

    if (screen == Screen::ReaderSpeed) {
        if (reader_streaming) {
            if (ev.key == Key::Ok) {
                speed_paused = !speed_paused;
                speed_next_ms = M5.millis() + speedIntervalMs();
            } else if (ev.key == Key::Up) speed_wpm = std::min(SPEED_WPM_MAX, speed_wpm + SPEED_WPM_STEP);
            else if (ev.key == Key::Down) speed_wpm = std::max(SPEED_WPM_MIN, speed_wpm - SPEED_WPM_STEP);
            else if (ev.key == Key::Left || ev.key == Key::Right) {
                int mode = static_cast<int>(speed_mode);
                mode = ev.key == Key::Left ? (mode + 2) % 3 : (mode + 1) % 3;
                speed_mode = static_cast<SpeedMode>(mode);
                std::string ignored;
                if (!loadReaderStreamSpeedUnit(&ignored)) reader_stream_status = "SPEED: read failed";
            } else if (ev.key == Key::Home || ev.key == Key::Back) {
                reader_stream_offset = reader_stream_speed_offset;
                reader_stream_history.clear();
                std::string ignored;
                loadReaderStreamPage(&ignored);
                saveReaderBookmark();
                saveReaderState();
                screen = Screen::ReaderView;
                blockInput(250);
            }
            dirty = true;
            return;
        }
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
            saveReaderState();
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
        else if (ev.key == Key::One) {
            note_input.clear();
            note_edit_existing = false;
            screen = Screen::NotesEdit;
            blockInput(300);
        }
        else if (ev.key == Key::Ok) {
            if (notes_cursor == 0) {
                note_input.clear();
                note_edit_existing = false;
                screen = Screen::NotesEdit;
                blockInput(300);
            } else {
                std::string err;
                if (loadSelectedNote(&err)) {
                    last_note_name = active_note_name;
                    screen = Screen::NotesView;
                    blockInput(300);
                } else {
                    showMessage("Note failed", err.empty() ? "open" : err, MessageReturn::Notes);
                }
            }
        } else if (ev.key == Key::Backspace && notes_cursor > 0 && notes_cursor <= static_cast<int>(notes.size())) {
            screen = Screen::NotesDeleteConfirm;
            blockInput(300);
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
        else if (ev.key == Key::One) {
            std::string err;
            if (loadRawNoteForEdit(&err)) {
                screen = Screen::NotesEdit;
                blockInput(300);
            } else {
                showMessage("Edit failed", err.empty() ? "open" : err, MessageReturn::Notes);
            }
        }
        else if (ev.key == Key::Home || ev.key == Key::Back) {
            screen = Screen::MusicList;
            blockInput(250);
        }
        dirty = true;
        return;
    }

    if (screen == Screen::NotesEdit) {
        if (ev.key == Key::Ok) {
            if (note_input.empty()) {
                screen = Screen::NotesList;
            } else {
                std::string name;
                std::string err;
                bool ok = note_edit_existing ? saveExistingNote(&err) : saveNewNote(&name, &err);
                if (ok) {
                    appendInboxEvent(note_edit_existing ? "NOTE EDIT" : "NOTE", note_edit_existing ? active_note_name : name);
                    showMessage("Note saved", note_edit_existing ? active_note_name : name, MessageReturn::Notes);
                } else {
                    showMessage("Save failed", err.empty() ? "write" : err, MessageReturn::Notes);
                }
                note_edit_existing = false;
            }
            blockInput(400);
        } else if (ev.key == Key::Backspace) {
            if (!note_input.empty()) note_input.pop_back();
        } else if (ev.key == Key::Home || ev.key == Key::Back) {
            note_input.clear();
            note_edit_existing = false;
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

    if (screen == Screen::NotesDeleteConfirm) {
        if (ev.key == Key::Ok) {
            std::string path = selectedNotePath();
            std::string name = (notes_cursor > 0 && notes_cursor <= static_cast<int>(notes.size())) ? notes[notes_cursor - 1] : "";
            if (!path.empty() && unlink(path.c_str()) == 0) {
                scanNotes();
                showMessage("Note deleted", name, MessageReturn::Notes);
            } else {
                showMessage("Delete failed", name.empty() ? "note" : name, MessageReturn::Notes);
            }
            blockInput(400);
        } else if (ev.key == Key::Home || ev.key == Key::Back) {
            screen = Screen::NotesList;
            blockInput(250);
        }
        dirty = true;
        return;
    }

    if (screen == Screen::RecorderList) {
        const int total = static_cast<int>(recordings.size()) + 1;
        if (ev.key == Key::Up) recorder_cursor = std::max(0, recorder_cursor - 1);
        else if (ev.key == Key::Down) recorder_cursor = std::min(total - 1, recorder_cursor + 1);
        else if (ev.key == Key::Left) recorder_cursor = std::max(0, recorder_cursor - 3);
        else if (ev.key == Key::Right) recorder_cursor = std::min(total - 1, recorder_cursor + 3);
        else if (ev.key == Key::Ok) {
            std::string err;
            if (recorder_cursor == 0) {
                if (!startRecording(&err)) {
                    showMessage("Record failed", err.empty() ? "start" : err);
                }
            } else {
                if (!startRecordingPlayback(&err)) {
                    showMessage(err == "bad rec" ? "BAD REC" : "Play failed", err.empty() ? "open" : err, MessageReturn::Recorder);
                }
            }
            blockInput(300);
        }
        else if (ev.key == Key::Backspace) {
            if (recorder_cursor > 0 && recorder_cursor <= static_cast<int>(recordings.size())) {
                pending_delete_name = recordings[recorder_cursor - 1];
                pending_delete_path = recordingPathByName(pending_delete_name);
                screen = Screen::RecorderDeleteConfirm;
            } else {
                showMessage("Delete skipped", "select recording", MessageReturn::Recorder);
            }
            blockInput(300);
        }
        else if (ev.key == Key::Home || ev.key == Key::Back) {
            screen = Screen::Launcher;
            blockInput(250);
        }
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

    if (screen == Screen::RecorderDeleteConfirm) {
        if (ev.key == Key::Ok) {
            if (!pending_delete_path.empty() && unlink(pending_delete_path.c_str()) == 0) {
                scanRecordings();
                showMessage("Rec deleted", pending_delete_name, MessageReturn::Recorder);
            } else {
                showMessage("Delete failed", pending_delete_name + "\n" + std::strerror(errno), MessageReturn::Recorder);
            }
            pending_delete_path.clear();
            pending_delete_name.clear();
            blockInput(400);
        } else if (ev.key == Key::Home || ev.key == Key::Back) {
            pending_delete_path.clear();
            pending_delete_name.clear();
            screen = Screen::RecorderList;
            blockInput(250);
        }
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
                    timer_preset_index = (timer_preset_index + 1) % (sizeof(TIMER_PRESETS) / sizeof(TIMER_PRESETS[0]));
                    applyTimerPreset();
                }
            } else if (time_mode == TimeMode::Alarm) {
                time_set_field = static_cast<TimeSetField>((static_cast<int>(time_set_field) + 1) % 3);
            } else {
                time_set_field = static_cast<TimeSetField>((static_cast<int>(time_set_field) + 1) % 3);
            }
        } else if (ev.key == Key::Up) {
            int step = timeFieldStepSeconds();
            if (time_mode == TimeMode::Clock) { clock_seconds = (elapsedClockSeconds() + step) % 86400; clock_base_ms = M5.millis(); }
            else if (time_mode == TimeMode::Timer && !timer_running) { timer_seconds = std::min(23 * 3600 + 59 * 60 + 59, timer_seconds + 60); timer_remaining_ms = timer_seconds * 1000UL; timer_done = false; }
            else if (time_mode == TimeMode::Alarm) { alarm_seconds = (alarm_seconds + step) % 86400; alarm_ringing = false; }
        } else if (ev.key == Key::Down) {
            int step = timeFieldStepSeconds();
            if (time_mode == TimeMode::Clock) { clock_seconds = (elapsedClockSeconds() + 86400 - step) % 86400; clock_base_ms = M5.millis(); }
            else if (time_mode == TimeMode::Timer && !timer_running) { timer_seconds = std::max(1, timer_seconds - 60); timer_remaining_ms = timer_seconds * 1000UL; timer_done = false; }
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
        else if (ev.key == Key::One) {
            scanFiles(MOUNT_POINT);
            blockInput(220);
        }
        else if (ev.key == Key::Two) {
            std::string err;
            if (createUnsupportedTestFile(&err)) {
                scanFiles(CONFIG_DIR);
                showMessage("Test file", "/CARDPTR/X.BIN", MessageReturn::Files);
            } else {
                showMessage("Test failed", err.empty() ? "write failed" : err, MessageReturn::Files);
            }
            blockInput(350);
        }
        else if (ev.key == Key::Ok && !file_entries.empty()) {
            std::string err;
            if (!openFileEntry(file_entries[files_cursor], &err)) {
                showMessage("Open failed", err.empty() ? "unsupported" : err, MessageReturn::Files);
            }
            blockInput(350);
        } else if (ev.key == Key::Backspace && !file_entries.empty()) {
            const auto& e = file_entries[files_cursor];
            if (e.is_dir) {
                showMessage("Delete skipped", "folders disabled", MessageReturn::Files);
            } else {
                pending_delete_path = e.path;
                pending_delete_name = e.name;
                screen = Screen::FilesDeleteConfirm;
            }
            blockInput(300);
        } else if (ev.key == Key::Home || ev.key == Key::Back) {
            if (files_path == MOUNT_POINT) screen = Screen::Launcher;
            else scanFiles(parentPath(files_path));
            blockInput(250);
        }
        dirty = true;
        return;
    }

    if (screen == Screen::FilesInfo) {
        if (ev.key == Key::Ok || ev.key == Key::Home || ev.key == Key::Back) {
            screen = Screen::FilesList;
            blockInput(250);
        }
        dirty = true;
        return;
    }

    if (screen == Screen::FilesDeleteConfirm) {
        if (ev.key == Key::Ok) {
            if (!pending_delete_path.empty() && unlink(pending_delete_path.c_str()) == 0) {
                showMessage("Deleted", pending_delete_name, MessageReturn::Files);
                scanFiles(files_path);
            } else {
                showMessage("Delete failed", pending_delete_name + "\n" + std::strerror(errno), MessageReturn::Files);
            }
            pending_delete_path.clear();
            pending_delete_name.clear();
            blockInput(400);
        } else if (ev.key == Key::Home || ev.key == Key::Back) {
            pending_delete_path.clear();
            pending_delete_name.clear();
            screen = Screen::FilesList;
            blockInput(250);
        }
        dirty = true;
        return;
    }

    if (screen == Screen::Randomizer) {
        if (ev.key == Key::Ok) {
            static const char* results[] = {"YES", "NO", "MB"};
            random_result = results[esp_random() % 3];
            random_history.insert(random_history.begin(), random_result);
            if (random_history.size() > 5) random_history.pop_back();
            blockInput(220);
        } else if (ev.key == Key::Home || ev.key == Key::Back) {
            screen = Screen::Launcher;
            blockInput(250);
        }
        dirty = true;
        return;
    }

    if (screen == Screen::HabitsList) {
        if (ev.key == Key::Up && !habits.empty()) habits_cursor = std::max(0, habits_cursor - 1);
        else if (ev.key == Key::Down && !habits.empty()) habits_cursor = std::min(static_cast<int>(habits.size()) - 1, habits_cursor + 1);
        else if (ev.key == Key::Ok && habits.empty() && disabled_habit_count > 0) {
            std::string err;
            if (!restoreAllHabits(&err)) showMessage("Restore failed", err.empty() ? "storage" : err, MessageReturn::Habits);
            else scanHabits();
            blockInput(300);
        } else if (ev.key == Key::Ok && !habits.empty()) {
            habits[habits_cursor].done = !habits[habits_cursor].done;
            saveHabitLogForDay();
            if (habits[habits_cursor].done) appendInboxEvent("HABIT", habits[habits_cursor].title);
            blockInput(220);
        } else if (ev.key == Key::One) {
            saveHabitLogForDay();
            habit_day = std::min(9999, habit_day + 1);
            for (auto& h : habits) h.done = false;
            saveHabitState();
            saveHabitLogForDay();
            blockInput(300);
        } else if (ev.key == Key::Right || shortcutChar(ev) == 's') {
            saveHabitLogForDay();
            habit_stats_window = 7;
            screen = Screen::HabitsStats;
            blockInput(250);
        } else if (ev.key == Key::Left || shortcutChar(ev) == 'm') {
            habits_manage_cursor = 0;
            screen = Screen::HabitsManage;
            blockInput(250);
        } else if (ev.key == Key::Home || ev.key == Key::Back) {
            saveHabitLogForDay();
            screen = Screen::Launcher;
            blockInput(250);
        }
        dirty = true;
        return;
    }

    if (screen == Screen::HabitsStats) {
        if (ev.key == Key::Left || ev.key == Key::Right || ev.key == Key::Ok) {
            habit_stats_window = habit_stats_window == 7 ? 30 : (habit_stats_window == 30 ? 365 : 7);
            blockInput(220);
        } else if (ev.key == Key::Home || ev.key == Key::Back) {
            screen = Screen::HabitsList;
            blockInput(250);
        }
        dirty = true;
        return;
    }

    if (screen == Screen::HabitsManage) {
        if (ev.key == Key::Up) habits_manage_cursor = std::max(0, habits_manage_cursor - 1);
        else if (ev.key == Key::Down) habits_manage_cursor = std::min(3, habits_manage_cursor + 1);
        else if (ev.key == Key::Ok) {
            if (habits_manage_cursor == 0) {
                habit_input.clear();
                habit_edit_id.clear();
                habit_edit_renaming = false;
                screen = Screen::HabitsEdit;
            } else if (habits_manage_cursor == 1) {
                if (!habits.empty()) {
                    habit_edit_id = habits[habits_cursor].id;
                    habit_input = habits[habits_cursor].title;
                    habit_edit_renaming = true;
                    screen = Screen::HabitsEdit;
                }
            } else if (habits_manage_cursor == 2) {
                if (!habits.empty()) {
                    pending_habit_id = habits[habits_cursor].id;
                    pending_habit_name = habits[habits_cursor].title;
                    screen = Screen::HabitsDisableConfirm;
                }
            } else {
                screen = Screen::HabitsList;
            }
            blockInput(250);
        } else if (ev.key == Key::Home || ev.key == Key::Back) {
            screen = Screen::HabitsList;
            blockInput(250);
        }
        dirty = true;
        return;
    }

    if (screen == Screen::HabitsEdit) {
        if (ev.key == Key::Ok) {
            if (!habit_input.empty()) {
                if (habit_edit_renaming) renameHabit(habit_edit_id, habit_input);
                else appendHabit(habit_input);
                scanHabits();
            }
            habit_input.clear();
            habit_edit_id.clear();
            habit_edit_renaming = false;
            screen = Screen::HabitsList;
            blockInput(300);
        } else if (ev.key == Key::Backspace) {
            if (!habit_input.empty()) habit_input.pop_back();
        } else if (ev.key == Key::Home || ev.key == Key::Back) {
            habit_input.clear();
            habit_edit_id.clear();
            habit_edit_renaming = false;
            screen = Screen::HabitsManage;
            blockInput(250);
        } else if (ev.key == Key::None && ev.name && ev.name[0] && !ev.name[1]) {
            char c = ev.name[0];
            if (static_cast<unsigned char>(c) >= 32 && static_cast<unsigned char>(c) <= 126 && c != '|' && habit_input.size() < 32) {
                habit_input.push_back(c);
            }
        }
        dirty = true;
        return;
    }

    if (screen == Screen::HabitsDisableConfirm) {
        if (ev.key == Key::Ok) {
            if (!pending_habit_id.empty()) {
                disableHabit(pending_habit_id);
                scanHabits();
            }
            pending_habit_id.clear();
            pending_habit_name.clear();
            screen = Screen::HabitsList;
            blockInput(300);
        } else if (ev.key == Key::Home || ev.key == Key::Back) {
            pending_habit_id.clear();
            pending_habit_name.clear();
            screen = Screen::HabitsManage;
            blockInput(250);
        }
        dirty = true;
        return;
    }

    if (screen == Screen::Settings) {
        if (settings_cast_editing) {
            if (ev.key == Key::Back || ev.key == Key::Home) {
                settings_cast_editing = false;
                settings_cast_edit_buffer.clear();
                blockInput(220);
            } else if (ev.key == Key::Ok) {
                if (settings_cast_edit_host) {
                    if (!settings_cast_edit_buffer.empty()) setCastHost(settings_cast_edit_buffer);
                    saveConfig();
                } else {
                    int parsed = settings_cast_edit_buffer.empty() ? static_cast<int>(cast_port) : std::atoi(settings_cast_edit_buffer.c_str());
                    if (parsed > 0 && parsed <= 65535) {
                        cast_port = static_cast<uint16_t>(parsed);
                        saveConfig();
                    } else {
                        showMessage("CAST PORT", "bad value");
                    }
                }
                settings_cast_editing = false;
                settings_cast_edit_buffer.clear();
                blockInput(220);
            } else if (ev.key == Key::Backspace) {
                if (!settings_cast_edit_buffer.empty()) settings_cast_edit_buffer.pop_back();
                blockInput(120);
            } else if (ev.key == Key::None && ev.name && ev.name[0] && !ev.name[1]) {
                char c = ev.name[0];
                if (settings_cast_edit_host) {
                    if (isCastHostChar(c) && settings_cast_edit_buffer.size() < CAST_MAX_HOST) {
                        settings_cast_edit_buffer.push_back(c);
                    }
                } else if (std::isdigit(static_cast<unsigned char>(c)) && settings_cast_edit_buffer.size() < 5) {
                    settings_cast_edit_buffer.push_back(c);
                }
                blockInput(80);
            }
            dirty = true;
            return;
        }
        if (ev.key == Key::Up) settings_cursor = std::max(0, settings_cursor - 1);
        else if (ev.key == Key::Down) settings_cursor = std::min(8, settings_cursor + 1);
        else if (ev.key == Key::Left || ev.key == Key::Right || ev.key == Key::Ok) {
            int dir = ev.key == Key::Left ? -1 : 1;
            if (settings_cursor == 0) {
                theme_mode = static_cast<ThemeMode>((static_cast<int>(theme_mode) + (dir < 0 ? 3 : 1)) % 4);
                power_save = false;
            } else if (settings_cursor == 1) {
                sound_mode = static_cast<SoundMode>((static_cast<int>(sound_mode) + (dir < 0 ? 4 : 1)) % 5);
                power_save = false;
            } else if (settings_cursor == 2) {
                timeout_mode = static_cast<TimeoutMode>((static_cast<int>(timeout_mode) + (dir < 0 ? 2 : 1)) % 3);
                power_save = false;
            } else if (settings_cursor == 3) {
                power_save = !power_save;
                applyPowerSavePreset();
            } else if (settings_cursor == 4 && ev.key == Key::Ok) {
                settings_cast_editing = true;
                settings_cast_edit_host = true;
                settings_cast_edit_buffer = cast_host;
            } else if (settings_cursor == 5 && ev.key == Key::Ok) {
                settings_cast_editing = true;
                settings_cast_edit_host = false;
                settings_cast_edit_buffer = std::to_string(static_cast<int>(cast_port));
            } else if (settings_cursor == 6 && ev.key == Key::Ok) {
                uint64_t total = 0;
                uint64_t free_b = 0;
                bool ok = manualSdReprobe() && sdUsage(&total, &free_b) && total >= free_b;
                if (ok) {
                    showMessage("SD REPROBE", "OK\nFREE " + formatBytes(free_b) + "\nUSED " + formatBytes(total - free_b));
                } else {
                    showMessage("SD REPROBE", "SD not ready");
                }
            } else if (settings_cursor == 7 && ev.key == Key::Ok) {
                const esp_app_desc_t* app = esp_app_get_description();
                char buf[160];
                snprintf(buf, sizeof(buf), "Pocket OS v%s\nBUILD %s\nPASS %s",
                         app ? app->version : "-",
                         app ? app->date : __DATE__,
                         connection_ap_password);
                showMessage("ABOUT", buf);
            } else if (settings_cursor == 8 && ev.key == Key::Ok) {
                screen = Screen::Connections;
            }
            if (settings_cursor >= 0 && settings_cursor <= 3) saveConfig();
            if (settings_cursor == 4 || settings_cursor == 5) saveConfig();
            blockInput(220);
        } else if (ev.key == Key::Home || ev.key == Key::Back) {
            screen = Screen::Launcher;
            blockInput(250);
        }
        dirty = true;
        return;
    }

    if (screen == Screen::Connections) {
        if (ev.key == Key::Ok) {
            if (!connection_wifi_on || !connection_http_on) {
                char err[64] = {};
                if (!startConnections(err, sizeof(err))) {
                    setConnectionStatus("start", err[0] ? err : "failed");
                }
            }
            blockInput(400);
        } else if (ev.key == Key::Home || ev.key == Key::Back) {
            if (connection_wifi_on || connection_http_on) stopConnections();
            screen = Screen::Launcher;
            blockInput(250);
        }
        dirty = true;
        return;
    }

    if (screen == Screen::Message) {
        if (M5.millis() < message_hold_until_ms) return;
        if (ev.key == Key::Home || ev.key == Key::Back || ev.key == Key::Ok) {
            const MessageReturn target = message_return;
            message_return = MessageReturn::Launcher;
            if (target == MessageReturn::Music) screen = Screen::MusicList;
            else if (target == MessageReturn::Notes) { scanNotes(); screen = Screen::NotesList; }
            else if (target == MessageReturn::Files) screen = Screen::FilesList;
            else if (target == MessageReturn::Recorder) { scanRecordings(); screen = Screen::RecorderList; }
            else if (target == MessageReturn::Habits) { scanHabits(); screen = Screen::HabitsList; }
            else screen = Screen::Launcher;
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
    loadConfig();
    loadReaderState();
    drawSplash();
    last_input_ms = M5.millis();
    dirty = true;

    while (true) {
        M5.update();
        KeyEvent ev = pollKey();
        handleKey(ev);
        processPersistence();
        processConnectionUploadOps();
        processConnectionTimeSync();
        if (connection_dirty) {
            connection_dirty = false;
            dirty = true;
        }
        uint32_t now = M5.millis();
        if (screen == Screen::Launcher && now < ui_anim_until_ms && now - ui_anim_last_frame_ms >= 33) {
            ui_anim_last_frame_ms = now;
            dirty = true;
        }
        if (!display_off && now - marquee_last_frame_ms >= 180) {
            if (screen == Screen::MusicList ||
                screen == Screen::MusicInfo || screen == Screen::MusicPlaying ||
                screen == Screen::ReaderList || screen == Screen::NotesList ||
                screen == Screen::RecorderList || screen == Screen::FilesList) {
                marquee_last_frame_ms = now;
                dirty = true;
            }
        }
        if (connection_upload_active) {
            last_input_ms = M5.millis();
            if (display_off || display_dim) wakeDisplay();
            dirty = true;
        }
        drawIfDirty();
        processMusicAutostart();
        updateAudio();
        updateRecording();
        updateRecordingPlayback();
        updateSpeedReader();
        updateTimeApp();
        updatePower();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
