#pragma once

#define FEATURE_MUSIC 1
#define FEATURE_RECORDER 1
#define FEATURE_NOTES 1
#define FEATURE_READER 1
#define FEATURE_CLOCK 1
#define FEATURE_NETWORK 1
#define FEATURE_WEB_FILE_MANAGER 1
#define FEATURE_RANDOMIZER 1
#define FEATURE_BROWSER 1
#define FEATURE_AI_TEXT 1
#define FEATURE_AI_VOICE 0
#define FEATURE_PAYMENTS_INFO 1
#define FEATURE_INPUT_DIAGNOSTICS 1
#define FEATURE_SYSTEM_INFO 1

// Enable ultra-safe boot diagnostics for first-device bring-up.
// Keeps firmware in a minimal state and avoids app/UI/SPI-heavy init paths.
#define FEATURE_ULTRA_SAFE_BOOT 1

#define FEATURE_SAFE_BOOT 1

#define FIRMWARE_VERSION "v0.1.4-safe-launcher"
