#pragma once

#include <cstdint>
#include <string>

#include "gnss_service.h"

enum class JourneyStatus { Idle, WaitingForFix, Recording, Saved, Error };

class JourneyService {
public:
    bool start(uint32_t now_ms, const char** error = nullptr);
    void appendIfDue(const GnssFix& fix, uint32_t now_ms);
    void stop();

    bool active() const { return active_; }
    JourneyStatus status() const { return status_; }
    const std::string& id() const { return id_; }
    const std::string& detail() const { return detail_; }
    uint32_t pointCount() const { return point_count_; }
    double distanceMeters() const { return distance_m_; }
    uint32_t elapsedSeconds(uint32_t now_ms) const;
    double paceSecondsPerKm(uint32_t now_ms) const;

private:
    void fail(const char* detail);

    bool active_ = false;
    JourneyStatus status_ = JourneyStatus::Idle;
    std::string id_;
    std::string detail_ = "READY";
    void* track_file_ = nullptr;
    uint32_t started_ms_ = 0;
    uint32_t last_log_ms_ = 0;
    uint32_t point_count_ = 0;
    double distance_m_ = 0.0;
    double last_latitude_ = 0.0;
    double last_longitude_ = 0.0;
    bool has_last_point_ = false;
};
