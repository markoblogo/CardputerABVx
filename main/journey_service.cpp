#include "journey_service.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <sys/stat.h>

namespace {
constexpr const char* JOURNEYS_DIR = "/sdcard/journeys";
constexpr uint32_t LOG_INTERVAL_MS = 30000;
constexpr double PI = 3.14159265358979323846;

FILE* trackFile(void* handle)
{
    return static_cast<FILE*>(handle);
}

double haversineMeters(double lat_a, double lon_a, double lat_b, double lon_b)
{
    const double lat_delta = (lat_b - lat_a) * PI / 180.0;
    const double lon_delta = (lon_b - lon_a) * PI / 180.0;
    const double a = std::sin(lat_delta / 2.0) * std::sin(lat_delta / 2.0) +
        std::cos(lat_a * PI / 180.0) * std::cos(lat_b * PI / 180.0) *
        std::sin(lon_delta / 2.0) * std::sin(lon_delta / 2.0);
    return 6371000.0 * 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
}
}  // namespace

bool JourneyService::start(uint32_t now_ms, const char** error)
{
    if (active_) return true;
    if (mkdir(JOURNEYS_DIR, 0775) != 0 && errno != EEXIST) {
        if (error) *error = "journeys directory";
        return false;
    }

    char folder[64] = {};
    bool folder_created = false;
    for (int number = 1; number <= 9999; ++number) {
        snprintf(folder, sizeof(folder), "%s/J%04d", JOURNEYS_DIR, number);
        if (mkdir(folder, 0775) == 0) {
            folder_created = true;
            break;
        }
        if (errno != EEXIST) break;
    }
    if (!folder_created) {
        if (error) *error = "journey folder";
        return false;
    }

    char track_path[96] = {};
    snprintf(track_path, sizeof(track_path), "%s/TRACK.CSV", folder);
    FILE* file = fopen(track_path, "w");
    if (!file) {
        if (error) *error = "track open";
        return false;
    }
    if (fprintf(file, "ms,lat,lon,alt,speed,fix,sats\n") < 0 || fflush(file) != 0) {
        fclose(file);
        if (error) *error = "track header";
        return false;
    }

    id_ = folder + std::strlen(JOURNEYS_DIR) + 1;
    track_file_ = file;
    active_ = true;
    status_ = JourneyStatus::WaitingForFix;
    detail_ = "WAITING FOR FIX";
    started_ms_ = now_ms;
    last_log_ms_ = 0;
    point_count_ = 0;
    distance_m_ = 0.0;
    has_last_point_ = false;
    return true;
}

void JourneyService::appendIfDue(const GnssFix& fix, uint32_t now_ms)
{
    if (!active_ || !track_file_ || !fix.valid) return;
    if (last_log_ms_ && now_ms - last_log_ms_ < LOG_INTERVAL_MS) return;

    FILE* file = trackFile(track_file_);
    const uint32_t elapsed_ms = now_ms - started_ms_;
    if (has_last_point_) distance_m_ += haversineMeters(last_latitude_, last_longitude_, fix.latitude, fix.longitude);
    if (fprintf(file, "%lu,%.6f,%.6f,%.1f,%.2f,1,%d\n",
                static_cast<unsigned long>(elapsed_ms), fix.latitude, fix.longitude,
                fix.altitude_m, fix.speed_mps, fix.satellites) < 0 || fflush(file) != 0) {
        fail("TRACK WRITE FAILED");
        return;
    }
    last_log_ms_ = now_ms;
    last_latitude_ = fix.latitude;
    last_longitude_ = fix.longitude;
    has_last_point_ = true;
    ++point_count_;
    status_ = JourneyStatus::Recording;
    detail_ = "LOGGING " + std::to_string(point_count_) + " POINTS";
}

void JourneyService::stop()
{
    if (track_file_) {
        fflush(trackFile(track_file_));
        fclose(trackFile(track_file_));
        track_file_ = nullptr;
    }
    active_ = false;
    status_ = JourneyStatus::Saved;
    detail_ = point_count_ ? "SAVED " + std::to_string(point_count_) + " POINTS" : "SAVED: NO FIX";
}

void JourneyService::fail(const char* detail)
{
    if (track_file_) {
        fclose(trackFile(track_file_));
        track_file_ = nullptr;
    }
    active_ = false;
    status_ = JourneyStatus::Error;
    detail_ = detail;
}

uint32_t JourneyService::elapsedSeconds(uint32_t now_ms) const
{
    return active_ ? (now_ms - started_ms_) / 1000 : 0;
}

double JourneyService::paceSecondsPerKm(uint32_t now_ms) const
{
    if (distance_m_ < 10.0) return 0.0;
    return static_cast<double>(elapsedSeconds(now_ms)) * 1000.0 / distance_m_;
}
