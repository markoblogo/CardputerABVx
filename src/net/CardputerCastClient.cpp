#include "CardputerCastClient.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>

CardputerCastClient::CardputerCastClient(String host, uint16_t port)
  : host_(host), port_(port) {}

void CardputerCastClient::setEndpoint(String host, uint16_t port) {
  host_ = host;
  port_ = port;
}

String CardputerCastClient::baseUrl() const {
  const String defaultHost = "192.168.4.1";
  const uint16_t defaultPort = 3000;
  const String host = host_.length() ? host_ : defaultHost;
  return String("http://") + host + ":" + String(port_ ? port_ : defaultPort);
}

bool CardputerCastClient::requestJson(
  const String& path,
  bool usePost,
  const String& body,
  int timeoutMs,
  String& response,
  int& code,
  String& err
) {
  HTTPClient http;
  const String url = baseUrl() + path;
  if (!http.begin(url)) {
    err = "http init failed";
    code = 0;
    return false;
  }
  http.setTimeout(timeoutMs);
  if (usePost) http.addHeader("Content-Type", "application/json");
  code = usePost ? http.POST(body) : http.GET();
  response = code > 0 ? http.getString() : String();
  http.end();
  if (code <= 0) {
    err = "request failed";
    return false;
  }
  return true;
}

bool CardputerCastClient::parseState(const String& sourceValue, String& outState, bool& outPlaying) const {
  if (sourceValue.length() == 0) {
    outState = "stopped";
    outPlaying = false;
    return false;
  }
  outState = sourceValue;
  outPlaying = sourceValue.equalsIgnoreCase("playing") || sourceValue.equalsIgnoreCase("play");
  return true;
}

bool CardputerCastClient::parseStatus(const String& response, CardputerCastStatus& out) const {
  JsonDocument doc;
  if (deserializeJson(doc, response) != DeserializationError::Ok) {
    out.connected = false;
    out.ok = false;
    out.error = "invalid_json";
    return false;
  }

  out.connected = true;
  out.raw = response;
  if (doc["time"].is<uint32_t>()) out.timeMs = doc["time"].as<uint32_t>();
  if (doc["time"].is<uint64_t>()) out.timeMs = static_cast<uint32_t>(doc["time"].as<uint64_t>());
  if (doc["ok"].is<bool>()) out.ok = doc["ok"].as<bool>();
  else out.ok = true;
  if (!out.ok) {
    out.state = doc["state"] | "stopped";
    out.error = String(doc["error"] | "error");
    out.source = String(doc["source"] | "ytmamp");
    return true;
  }

  String state;
  bool playing = false;
  bool stateParsed = false;
  const char* stateValue = doc["state"];
  if (stateValue && *stateValue) {
    stateParsed = parseState(String(stateValue), state, playing);
  }
  if (!stateParsed) {
    bool legacyPlaying = doc["playing"] | false;
    stateParsed = true;
    state = legacyPlaying ? "playing" : "stopped";
    playing = legacyPlaying;
  }
  out.state = state;
  out.connected = true;

  out.playing = playing;
  out.source = String(doc["source"] | "ytmamp");

  const char* error = doc["error"];
  if (error) out.error = error;

  if (doc["track"].is<JsonObject>()) {
    const JsonObject track = doc["track"];
    out.track.trackId = String(track["track_id"] | "");
    out.track.title = String(track["title"] | "");
    out.track.artist = String(track["artist"] | "");
    out.track.album = String(track["album"] | "");
    out.track.durationMs = track["duration_ms"] | 0u;
    out.track.positionMs = track["position_ms"] | 0u;
    out.volume = doc["volume"] | out.volume;
    out.hasTrack = out.track.title.length() || out.track.artist.length() || out.track.album.length() || out.track.trackId.length();
  } else {
    out.track.trackId = "";
    out.track.title = String(doc["title"] | "");
    out.track.artist = String(doc["artist"] | "");
    out.track.album = String(doc["album"] | "");
    out.track.durationMs = doc["duration_ms"] | 0u;
    out.track.positionMs = doc["position_ms"] | 0u;
    out.volume = doc["volume"] | out.volume;
    out.hasTrack = out.track.title.length() || out.track.artist.length() || out.track.album.length();
  }

  if (out.track.trackId.isEmpty() && !doc["track_id"].isNull()) {
    out.track.trackId = String(doc["track_id"]);
  }
  if (out.track.durationMs == 0 && doc["duration"].is<uint32_t>()) out.track.durationMs = doc["duration"];
  if (out.track.durationMs == 0 && doc["duration_ms"].is<uint64_t>()) out.track.durationMs = static_cast<uint32_t>(doc["duration_ms"].as<uint64_t>());
  if (out.track.positionMs == 0 && doc["position"].is<uint32_t>()) out.track.positionMs = doc["position"];
  if (out.track.positionMs == 0 && doc["position_ms"].is<uint64_t>()) out.track.positionMs = static_cast<uint32_t>(doc["position_ms"].as<uint64_t>());

  return true;
}

bool CardputerCastClient::getStatus(CardputerCastStatus& out) {
  String response;
  int code = 0;
  String err;

  const String paths[] = { "/api/cast/status", "/cast/status" };
  for (uint8_t i = 0; i < 2; ++i) {
    if (!requestJson(paths[i], false, String(), 1000, response, code, err)) continue;
    if (code < 200 || code >= 300) continue;
    if (parseStatus(response, out)) return true;
  }

  out.connected = false;
  out.ok = false;
  out.error = err.length() ? err : String("request_failed_") + String(code);
  return false;
}

bool CardputerCastClient::postCommand(const String& action, CardputerCastStatus* responseStatus, const String& body) {
  const String bodyToSend = body.length() ? body : String("{\"action\":\"") + action + "\"}";
  String response;
  int code = 0;
  String err;

  const String apiPath = "/api/cast/cmd";
  if (requestJson(apiPath, true, bodyToSend, 1200, response, code, err) && code >= 200 && code < 300) {
    if (responseStatus) parseStatus(response, *responseStatus);
    return true;
  }

  const String legacyPath = String("/cast/") + action;
  if (requestJson(legacyPath, false, String(), 1200, response, code, err) && code >= 200 && code < 300) {
    if (responseStatus) {
      responseStatus->connected = true;
      responseStatus->ok = true;
      responseStatus->state = "playing";
      responseStatus->raw = response.length() ? response : String("{\"ok\":true}");
    }
    return true;
  }
  if (responseStatus) {
    responseStatus->connected = false;
    responseStatus->ok = false;
    responseStatus->error = err.length() ? err : String("request_failed_") + String(code);
  }
  return false;
}
