#pragma once

#include <cstddef>
#include <cstdint>

struct GnssFix {
    bool seen = false;
    bool valid = false;
    double latitude = 0.0;
    double longitude = 0.0;
    double altitude_m = 0.0;
    double speed_mps = 0.0;
    int satellites = 0;
    uint32_t last_sentence_ms = 0;
    uint32_t sentence_count = 0;
};

enum class GnssStatus { Off, Searching, NoFix, Fix, Stale, Error };

class GnssService {
public:
    bool begin(const char** error = nullptr);
    void poll(uint32_t now_ms);

    const GnssFix& fix() const { return fix_; }
    GnssStatus status(uint32_t now_ms) const;

private:
    void parseSentence(char* line, uint32_t now_ms);

    bool ready_ = false;
    bool error_ = false;
    char line_[128] = {};
    size_t line_len_ = 0;
    GnssFix fix_;
};
