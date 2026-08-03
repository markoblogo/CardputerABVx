#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <String.h>

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

  String host_;
  uint16_t port_;
};
