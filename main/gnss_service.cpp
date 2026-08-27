#include "gnss_service.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include <driver/uart.h>

namespace {
constexpr uart_port_t GNSS_UART = UART_NUM_1;
constexpr int GNSS_RX_PIN = 15;
constexpr int GNSS_TX_PIN = 13;
constexpr int GNSS_BAUD = 115200;
constexpr uint32_t GNSS_STALE_MS = 3000;

int hexValue(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

bool hasValidChecksum(char* line)
{
    if (!line || line[0] != '$') return false;
    char* checksum = std::strchr(line, '*');
    if (!checksum || !checksum[1] || !checksum[2]) return false;
    const int high = hexValue(checksum[1]);
    const int low = hexValue(checksum[2]);
    if (high < 0 || low < 0) return false;
    uint8_t calculated = 0;
    for (char* cursor = line + 1; cursor < checksum; ++cursor) calculated ^= static_cast<uint8_t>(*cursor);
    *checksum = '\0';
    return calculated == static_cast<uint8_t>((high << 4) | low);
}

int splitFields(char* sentence, char** fields, int max_fields)
{
    int count = 0;
    fields[count++] = sentence + 1;
    for (char* cursor = sentence + 1; *cursor && count < max_fields; ++cursor) {
        if (*cursor == ',') {
            *cursor = '\0';
            fields[count++] = cursor + 1;
        }
    }
    return count;
}

bool hasSuffix(const char* value, const char* suffix)
{
    const size_t value_len = std::strlen(value);
    const size_t suffix_len = std::strlen(suffix);
    return value_len >= suffix_len && std::strcmp(value + value_len - suffix_len, suffix) == 0;
}

bool parseCoordinate(const char* value, const char* hemisphere, bool latitude, double* result)
{
    if (!value || !hemisphere || !result || !value[0] || !hemisphere[0]) return false;
    char* end = nullptr;
    const double encoded = std::strtod(value, &end);
    if (end == value || encoded < 0.0) return false;
    const int degrees = static_cast<int>(encoded / 100.0);
    const double minutes = encoded - degrees * 100.0;
    if (minutes < 0.0 || minutes >= 60.0) return false;
    double decimal = degrees + minutes / 60.0;
    if (latitude) {
        if (degrees > 90 || (hemisphere[0] != 'N' && hemisphere[0] != 'S')) return false;
        if (hemisphere[0] == 'S') decimal = -decimal;
    } else {
        if (degrees > 180 || (hemisphere[0] != 'E' && hemisphere[0] != 'W')) return false;
        if (hemisphere[0] == 'W') decimal = -decimal;
    }
    *result = decimal;
    return true;
}
}  // namespace

bool GnssService::begin(const char** error)
{
    if (ready_) return true;
    uart_config_t config = {};
    config.baud_rate = GNSS_BAUD;
    config.data_bits = UART_DATA_8_BITS;
    config.parity = UART_PARITY_DISABLE;
    config.stop_bits = UART_STOP_BITS_1;
    config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    config.source_clk = UART_SCLK_DEFAULT;

    esp_err_t result = uart_driver_install(GNSS_UART, 2048, 0, 0, nullptr, 0);
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        error_ = true;
        if (error) *error = "UART driver unavailable";
        return false;
    }
    result = uart_param_config(GNSS_UART, &config);
    if (result == ESP_OK) result = uart_set_pin(GNSS_UART, GNSS_TX_PIN, GNSS_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (result != ESP_OK) {
        error_ = true;
        if (error) *error = "UART configuration failed";
        return false;
    }
    static constexpr char VERSION_QUERY[] = "$PCAS06,0*1B\r\n";
    uart_write_bytes(GNSS_UART, VERSION_QUERY, sizeof(VERSION_QUERY) - 1);
    ready_ = true;
    return true;
}

void GnssService::poll(uint32_t now_ms)
{
    if (!ready_) return;
    char buffer[96];
    const int received = uart_read_bytes(GNSS_UART, reinterpret_cast<uint8_t*>(buffer), sizeof(buffer), 0);
    if (received < 0) {
        error_ = true;
        return;
    }
    fix_.byte_count += static_cast<uint32_t>(received);
    for (int index = 0; index < received; ++index) {
        const char value = buffer[index];
        if (value == '\r' || value == '\n') {
            if (line_len_) {
                line_[line_len_] = '\0';
                ++fix_.line_count;
                parseSentence(line_, now_ms);
                line_len_ = 0;
            }
        } else if (line_len_ + 1 < sizeof(line_)) {
            line_[line_len_++] = value;
        } else {
            line_len_ = 0;
        }
    }
}

void GnssService::parseSentence(char* line, uint32_t now_ms)
{
    if (!hasValidChecksum(line)) {
        ++fix_.checksum_error_count;
        return;
    }
    char* fields[16] = {};
    const int count = splitFields(line, fields, 16);
    if (!count) return;
    fix_.seen = true;
    fix_.last_sentence_ms = now_ms;
    ++fix_.sentence_count;

    if (hasSuffix(fields[0], "RMC") && count >= 8) {
        double latitude = 0.0;
        double longitude = 0.0;
        const bool valid = std::strcmp(fields[2], "A") == 0 &&
            parseCoordinate(fields[3], fields[4], true, &latitude) &&
            parseCoordinate(fields[5], fields[6], false, &longitude);
        fix_.valid = valid;
        if (valid) {
            fix_.latitude = latitude;
            fix_.longitude = longitude;
            fix_.speed_mps = std::strtod(fields[7], nullptr) * 0.514444;
        }
    } else if (hasSuffix(fields[0], "GGA") && count >= 10) {
        const int quality = std::atoi(fields[6]);
        fix_.satellites = std::atoi(fields[7]);
        if (quality > 0) {
            double latitude = 0.0;
            double longitude = 0.0;
            if (parseCoordinate(fields[2], fields[3], true, &latitude) &&
                parseCoordinate(fields[4], fields[5], false, &longitude)) {
                fix_.valid = true;
                fix_.latitude = latitude;
                fix_.longitude = longitude;
                fix_.altitude_m = std::strtod(fields[9], nullptr);
            }
        }
    }
}

GnssStatus GnssService::status(uint32_t now_ms) const
{
    if (error_) return GnssStatus::Error;
    if (!ready_) return GnssStatus::Off;
    if (!fix_.seen) return GnssStatus::Searching;
    if (now_ms - fix_.last_sentence_ms > GNSS_STALE_MS) return GnssStatus::Stale;
    return fix_.valid ? GnssStatus::Fix : GnssStatus::NoFix;
}
