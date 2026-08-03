#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <String.h>
#include <ArduinoJson.h>

struct CardputerCastTrack {
  String trackId;
  String title;
  String artist;
  String album;
  uint32_t durationMs = 0;
  uint32_t positionMs = 0;
};

struct CardputerCastStatus {
  bool ok = false;
  bool connected = false;
  bool playing = false;
  String state = "stopped";
  String source = "ytmamp";
  String error;
  uint32_t timeMs = 0;
  uint8_t volume = 0;
  CardputerCastTrack track;
  bool hasTrack = false;
  String raw;
};

class CardputerCastClient {
public:
  CardputerCastClient(String host = "192.168.4.1", uint16_t port = 3000);

  void setEndpoint(String host, uint16_t port);
  const String& host() const { return host_; }
  uint16_t port() const { return port_; }
  uint16_t lastLatencyMs() const { return lastLatencyMs_; }
  uint8_t lastAttemptCount() const { return lastAttemptCount_; }
  const String& lastError() const { return lastError_; }
  const String& lastPath() const { return lastPath_; }
  int lastStatusCode() const { return lastStatusCode_; }
  struct DebugSnapshot {
    String host;
    String path;
    int statusCode = 0;
    uint8_t attemptCount = 0;
    uint16_t latencyMs = 0;
    bool success = false;
    String error;
  };

  static DebugSnapshot latestDebugTrace();
  static String lastDebugPath();
  static int lastDebugStatusCode();
  static uint8_t lastDebugAttemptCount();
  static uint16_t lastDebugLatencyMs();
  static bool lastDebugSuccess();
  static String lastDebugError();

  bool getStatus(CardputerCastStatus& out);
  bool postCommand(const String& action, CardputerCastStatus* response = nullptr, const String& body = String());

private:
  String baseUrl() const;
  bool requestJson(
    const String& path,
    bool usePost,
    const String& body,
    int timeoutMs,
    String& response,
    int& code,
    String& err
  );
  bool parseStatus(const String& response, CardputerCastStatus& out) const;
  bool parseState(const String& stateValue, String& outState, bool& outPlaying) const;
  bool parseTrackObject(const JsonVariant& node, CardputerCastTrack& track, bool& hasTrack) const;
  bool parseMsValue(const JsonVariant& value, uint32_t& outMs) const;
  bool parseStateVariant(const JsonVariant& value, String& state, bool& playing) const;

  String host_;
  uint16_t port_;
  uint16_t lastLatencyMs_ = 0;
  uint8_t lastAttemptCount_ = 0;
  String lastError_ = "";
  String lastPath_;
  int lastStatusCode_ = 0;
  static void recordGlobalTrace(const String& host, const String& path, int statusCode, uint8_t attempts, uint16_t latencyMs, bool success, const String& error);
  static String endpointHost(uint16_t port, const String& host);
  static String endpointForDebug(const String& host, uint16_t port);
  static DebugSnapshot latestDebugTrace_;
};
