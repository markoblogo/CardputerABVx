#include "CardputerCastClient.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Arduino.h>
#include <cstdlib>

CardputerCastClient::CardputerCastClient(String host, uint16_t port)
  : host_(host), port_(port) {}

CardputerCastClient::DebugSnapshot CardputerCastClient::latestDebugTrace_ = {};

CardputerCastClient::DebugSnapshot CardputerCastClient::latestDebugTrace() {
  return latestDebugTrace_;
}

String CardputerCastClient::lastDebugPath() {
  return latestDebugTrace_.path;
}

int CardputerCastClient::lastDebugStatusCode() {
  return latestDebugTrace_.statusCode;
}

uint8_t CardputerCastClient::lastDebugAttemptCount() {
  return latestDebugTrace_.attemptCount;
}

uint16_t CardputerCastClient::lastDebugLatencyMs() {
  return latestDebugTrace_.latencyMs;
}

bool CardputerCastClient::lastDebugSuccess() {
  return latestDebugTrace_.success;
}

String CardputerCastClient::lastDebugError() {
  return latestDebugTrace_.error;
}

String CardputerCastClient::endpointForDebug(const String& host, uint16_t port) {
  const String endpointHost = host.length() ? host : String("192.168.4.1");
  return endpointHost + ":" + String(port ? port : 3000);
}

void CardputerCastClient::recordGlobalTrace(
  const String& host,
  const String& path,
  int statusCode,
  uint8_t attempts,
  uint16_t latencyMs,
  bool success,
  const String& error
) {
  latestDebugTrace_.host = host.length() ? host : endpointForDebug(String("192.168.4.1"), 3000);
  latestDebugTrace_.path = path;
  latestDebugTrace_.statusCode = statusCode;
  latestDebugTrace_.attemptCount = attempts;
  latestDebugTrace_.latencyMs = latencyMs;
  latestDebugTrace_.success = success;
  latestDebugTrace_.error = error.length() ? error : String(success ? "ok" : "error");
}

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
  const uint32_t startedMs = millis();
  lastError_ = "ok";
  lastAttemptCount_++;
  lastPath_ = path;
  const String url = baseUrl() + path;
  if (!http.begin(url)) {
    err = "http init failed";
    code = 0;
    lastError_ = err;
    lastLatencyMs_ = static_cast<uint16_t>(min<uint32_t>(millis() - startedMs, 65535));
    recordGlobalTrace(endpointForDebug(host_, port_), path, code, lastAttemptCount_, lastLatencyMs_, false, lastError_);
    return false;
  }
  http.setTimeout(timeoutMs);
  if (usePost) http.addHeader("Content-Type", "application/json");
  code = usePost ? http.POST(body) : http.GET();
  response = code > 0 ? http.getString() : String();
  http.end();
  if (code <= 0) {
    err = "request failed";
    lastError_ = err;
    lastLatencyMs_ = static_cast<uint16_t>(min<uint32_t>(millis() - startedMs, 65535));
    recordGlobalTrace(endpointForDebug(host_, port_), path, code, lastAttemptCount_, lastLatencyMs_, false, lastError_);
    return false;
  }
  lastLatencyMs_ = static_cast<uint16_t>(min<uint32_t>(millis() - startedMs, 65535));
  if (code >= 200 && code < 300) {
    recordGlobalTrace(endpointForDebug(host_, port_), path, code, lastAttemptCount_, lastLatencyMs_, true, "ok");
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

bool CardputerCastClient::parseTrackObject(const JsonVariant& node, CardputerCastTrack& track, bool& hasTrack) const {
  hasTrack = false;
  if (!node.is<JsonObject>()) {
    return false;
  }
  const JsonObject obj = node.as<JsonObject>();
  track.trackId = String(obj["track_id"] | obj["id"] | "");
  track.title = String(obj["title"] | "");
  track.artist = String(obj["artist"] | "");
  track.album = String(obj["album"] | "");
  if (!parseMsValue(obj["duration_ms"], track.durationMs)) parseMsValue(obj["duration"], track.durationMs);
  if (!parseMsValue(obj["position_ms"], track.positionMs)) parseMsValue(obj["position"], track.positionMs);
  hasTrack = track.title.length() || track.artist.length() || track.album.length() || track.trackId.length() || track.durationMs || track.positionMs;
  return hasTrack;
}

bool CardputerCastClient::parseMsValue(const JsonVariant& value, uint32_t& outMs) const {
  if (value.is<const char*>()) {
    const char* raw = value.as<const char*>();
    if (raw && raw[0]) {
      char* endp = nullptr;
      const long long parsed = strtoll(raw, &endp, 10);
      if (endp != nullptr && endp != raw) {
        if (parsed < 0) outMs = 0;
        else outMs = parsed > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(parsed);
        return true;
      }
    }
    return false;
  }
  if (value.is<uint32_t>()) {
    outMs = value.as<uint32_t>();
    return true;
  }
  if (value.is<uint64_t>()) {
    outMs = value.as<uint64_t>() > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(value.as<uint64_t>());
    return true;
  }
  if (value.is<float>()) {
    outMs = static_cast<uint32_t>(value.as<float>());
    return true;
  }
  if (value.is<double>()) {
    outMs = static_cast<uint32_t>(value.as<double>());
    return true;
  }
  return false;
}

bool CardputerCastClient::parseStateVariant(const JsonVariant& value, String& outState, bool& outPlaying) const {
  if (value.is<const char*>()) {
    return parseState(String(value.as<const char*>()), outState, outPlaying);
  }
  if (value.is<bool>()) {
    outPlaying = value.as<bool>();
    outState = outPlaying ? "playing" : "stopped";
    return true;
  }
  return false;
}

bool CardputerCastClient::parseStatus(const String& response, CardputerCastStatus& out) const {
  out = {};
  JsonDocument doc;
  if (deserializeJson(doc, response) != DeserializationError::Ok) {
    out.connected = false;
    out.ok = false;
    out.error = "invalid_json";
    return false;
  }

  out.connected = true;
  out.raw = response;
  parseMsValue(doc["time"], out.timeMs);
  if (out.timeMs == 0 && doc["time_ms"].is<uint32_t>()) out.timeMs = doc["time_ms"].as<uint32_t>();
  if (out.timeMs == 0 && doc["time_ms"].is<uint64_t>()) out.timeMs = static_cast<uint32_t>(doc["time_ms"].as<uint64_t>());
  if (out.timeMs == 0 && doc["timestamp"].is<uint32_t>()) out.timeMs = doc["timestamp"].as<uint32_t>();
  if (out.timeMs == 0 && doc["timestamp"].is<uint64_t>()) out.timeMs = static_cast<uint32_t>(doc["timestamp"].as<uint64_t>());
  if (out.timeMs == 0 && doc["time_sec"].is<uint32_t>()) out.timeMs = doc["time_sec"].as<uint32_t>() * 1000u;
  if (out.timeMs == 0 && doc["position_ms"].is<uint32_t>()) out.timeMs = doc["position_ms"].as<uint32_t>();
  if (doc["ok"].is<bool>()) out.ok = doc["ok"].as<bool>();
  else out.ok = true;
  if (!out.ok) {
    parseStateVariant(doc["state"], out.state, out.playing);
    if (out.state.isEmpty()) out.state = "stopped";
    out.error = String(doc["error"] | "error");
    out.source = String(doc["source"] | "ytmamp");
    return true;
  }

  String state;
  bool playing = false;
  bool stateParsed = false;
  stateParsed = parseStateVariant(doc["state"], state, playing);
  if (!stateParsed) {
    bool legacyPlaying = doc["playing"] | false;
    bool paused = doc["paused"] | false;
    bool isPlaying = doc["is_playing"] | false;
    if (doc["status"].is<const char*>()) {
      stateParsed = parseStateVariant(doc["status"], state, playing);
    } else if (doc["playbackState"].is<const char*>()) {
      stateParsed = parseStateVariant(doc["playbackState"], state, playing);
    } else {
      state = isPlaying ? "playing" : (legacyPlaying ? "playing" : (paused ? "paused" : "stopped"));
      playing = isPlaying || (!paused && legacyPlaying);
      stateParsed = true;
    }
  }
  out.state = state.isEmpty() ? "stopped" : state;
  out.connected = true;

  out.playing = playing;
  out.source = String(doc["source"] | "ytmamp");
  if (out.volume == 0) {
    uint32_t volumeValue = 0;
    parseMsValue(doc["volume"], volumeValue);
    if (volumeValue == 0) parseMsValue(doc["vol"], volumeValue);
    if (doc["level"].is<uint32_t>()) volumeValue = doc["level"].as<uint32_t>(); 
    out.volume = static_cast<uint8_t>(min<uint32_t>(volumeValue, 255));
    if (out.volume > 100 && out.volume <= 255 && doc["vol"]) {
      out.volume = static_cast<uint8_t>(min<uint32_t>(out.volume, 100));
    }
  }

  const char* error = doc["error"];
  if (error) out.error = error;
  if (doc["error"].is<const char*>() && out.error.isEmpty()) out.error = String(doc["error"].as<const char*>());

  if (doc["track"].is<JsonObject>()) {
    parseTrackObject(doc["track"], out.track, out.hasTrack);
  } else {
    if (doc["track"].is<const char*>()) {
      out.track.title = String(doc["track"].as<const char*>());
    }
    out.track.trackId = String(doc["track_id"] | out.track.trackId);
    if (out.track.title.isEmpty()) out.track.title = String(doc["title"] | "");
    out.track.artist = String(doc["artist"] | "");
    out.track.album = String(doc["album"] | "");
    if (!parseMsValue(doc["duration"], out.track.durationMs)) parseMsValue(doc["duration_ms"], out.track.durationMs);
    if (!parseMsValue(doc["position"], out.track.positionMs)) parseMsValue(doc["position_ms"], out.track.positionMs);
    out.hasTrack = out.track.title.length() || out.track.artist.length() || out.track.album.length() || out.track.trackId.length();
    if (out.track.durationMs == 0 && doc["track_duration"].is<uint32_t>()) out.track.durationMs = doc["track_duration"].as<uint32_t>();
    if (out.track.positionMs == 0 && doc["track_position"].is<uint32_t>()) out.track.positionMs = doc["track_position"].as<uint32_t>();
    if (out.track.durationMs == 0 && doc["duration_sec"].is<uint32_t>()) out.track.durationMs = doc["duration_sec"].as<uint32_t>() * 1000u;
    if (out.track.positionMs == 0 && doc["position_sec"].is<uint32_t>()) out.track.positionMs = doc["position_sec"].as<uint32_t>() * 1000u;
  }

  if (out.track.trackId.isEmpty() && !doc["track_id"].isNull()) {
    out.track.trackId = String(doc["track_id"] | "");
  }
  if (out.track.durationMs == 0 && doc["track"]["duration_ms"]) parseMsValue(doc["track"]["duration_ms"], out.track.durationMs);
  if (out.track.positionMs == 0 && doc["track"]["position_ms"]) parseMsValue(doc["track"]["position_ms"], out.track.positionMs);

  return true;
}

bool CardputerCastClient::getStatus(CardputerCastStatus& out) {
  String response;
  int code = 0;
  String err;
  const uint8_t attempts = 4;
  const int timeoutMs = 1500;
  const uint16_t baseDelayMs = 90;
  const uint16_t maxDelayMs = 720;
  lastLatencyMs_ = 0;
  lastAttemptCount_ = 0;
  lastError_.remove(0);
  lastStatusCode_ = 0;
  lastPath_.remove(0);

  const String paths[] = { "/api/cast/status", "/cast/status", "/status" };
  String endpointSummary;
  for (uint8_t i = 0; i < 3; ++i) {
    const String path = paths[i];
    String pathSummary;
    for (uint8_t attempt = 0; attempt < attempts; ++attempt) {
      if (requestJson(path, false, String(), timeoutMs, response, code, err) && code >= 200 && code < 300) {
        lastStatusCode_ = code;
        lastPath_ = path;
        if (parseStatus(response, out)) {
          lastError_ = err;
          recordGlobalTrace(endpointForDebug(host_, port_), path, code, lastAttemptCount_, lastLatencyMs_, true, err.length() ? err : "ok");
          return true;
        }
      }
      String reason = err.length() ? err : "status_parse_failed";
      recordGlobalTrace(endpointForDebug(host_, port_), path, code, lastAttemptCount_, lastLatencyMs_, false, reason);
      pathSummary += String("a") + String(attempt + 1) + String("=") + String(code) + ":" + reason + " ";
      if (code) {
        lastStatusCode_ = code;
      }
      if (attempt + 1 < attempts) {
        const uint16_t delayMs = min<uint16_t>(maxDelayMs, baseDelayMs << attempt);
        delay(delayMs);
      }
    }
    if (pathSummary.length()) endpointSummary += path + ": " + pathSummary;
  }

  out.connected = false;
  out.ok = false;
  out.error = endpointSummary.length() ? endpointSummary : String("request_failed_") + String(code);
  if (!endpointSummary.length() && err.length()) out.error = err;
  lastError_ = out.error;
  recordGlobalTrace(endpointForDebug(host_, port_), lastPath_, code, lastAttemptCount_, lastLatencyMs_, false, out.error);
  return false;
}

bool CardputerCastClient::postCommand(const String& action, CardputerCastStatus* responseStatus, const String& body) {
  const String bodyToSend = body.length() ? body : String("{\"action\":\"") + action + "\"}";
  String response;
  int code = 0;
  String err;
  const uint8_t attempts = 4;
  const int timeoutMs = 1500;
  const uint16_t baseDelayMs = 90;
  const uint16_t maxDelayMs = 720;
  lastLatencyMs_ = 0;
  lastAttemptCount_ = 0;
  lastError_.remove(0);
  lastStatusCode_ = 0;
  lastPath_.remove(0);

  const String apiPath = "/api/cast/cmd";
  String endpointSummary;
  String cmdSummary;
  for (uint8_t attempt = 0; attempt < attempts; ++attempt) {
    lastPath_ = apiPath;
    if (requestJson(apiPath, true, bodyToSend, timeoutMs, response, code, err) && code >= 200 && code < 300) {
      lastStatusCode_ = code;
      lastPath_ = apiPath;
      if (responseStatus) parseStatus(response, *responseStatus);
      lastError_ = err;
      recordGlobalTrace(endpointForDebug(host_, port_), apiPath, code, lastAttemptCount_, lastLatencyMs_, true, String(err.length() ? err : "ok"));
      return true;
    }
    String reason = err.length() ? err : "command_failed";
    recordGlobalTrace(endpointForDebug(host_, port_), apiPath, code, lastAttemptCount_, lastLatencyMs_, false, reason);
    cmdSummary += String("a") + String(attempt + 1) + String("=") + String(code) + ":" + reason + " ";
    if (code) {
      lastStatusCode_ = code;
    }
    if (attempt + 1 < attempts) {
      const uint16_t delayMs = min<uint16_t>(maxDelayMs, baseDelayMs << attempt);
      delay(delayMs);
    }
  }
  if (cmdSummary.length()) endpointSummary += apiPath + ": " + cmdSummary;

  const String legacyPath = String("/cast/") + action;
  String castSummary;
  for (uint8_t attempt = 0; attempt < attempts; ++attempt) {
    lastPath_ = legacyPath;
    if (requestJson(legacyPath, false, String(), timeoutMs, response, code, err) && code >= 200 && code < 300) {
      lastStatusCode_ = code;
      lastPath_ = legacyPath;
      if (responseStatus) {
        CardputerCastStatus fallback;
        if (parseStatus(response, fallback)) {
          *responseStatus = fallback;
        } else {
          responseStatus->connected = true;
          responseStatus->ok = true;
          responseStatus->state = "playing";
          responseStatus->playing = true;
          responseStatus->raw = response.length() ? response : String("{\"ok\":true}");
        }
      }
      recordGlobalTrace(endpointForDebug(host_, port_), legacyPath, code, lastAttemptCount_, lastLatencyMs_, true, String("ok"));
      return true;
    }
      recordGlobalTrace(endpointForDebug(host_, port_), legacyPath, code, lastAttemptCount_, lastLatencyMs_, false, err.length() ? err : "fallback_command_failed");
      String reason = err.length() ? err : "fallback_command_failed";
      castSummary += String("a") + String(attempt + 1) + String("=") + String(code) + ":" + reason + " ";
      if (code) {
        lastStatusCode_ = code;
      }
    if (attempt + 1 < attempts) {
      const uint16_t delayMs = min<uint16_t>(maxDelayMs, baseDelayMs << attempt);
      delay(delayMs);
    }
  }
  if (castSummary.length()) endpointSummary += String(" ") + legacyPath + ": " + castSummary;
  if (responseStatus) {
    if (getStatus(*responseStatus)) {
      recordGlobalTrace(endpointForDebug(host_, port_), lastPath_, lastStatusCode_, lastAttemptCount_, lastLatencyMs_, true, String("ok-fallback"));
      return true;
    }
  }
  const String legacyStatusPath = "/cast/status";
  for (uint8_t attempt = 0; attempt < attempts; ++attempt) {
    lastPath_ = legacyStatusPath;
    CardputerCastStatus fallback;
    String fallbackResponse;
    if (requestJson(legacyStatusPath, false, String(), timeoutMs, fallbackResponse, code, err) &&
        code >= 200 && code < 300 && parseStatus(fallbackResponse, fallback)) {
      lastStatusCode_ = code;
      lastPath_ = legacyStatusPath;
      if (responseStatus) {
        *responseStatus = fallback;
      }
      recordGlobalTrace(endpointForDebug(host_, port_), legacyStatusPath, code, lastAttemptCount_, lastLatencyMs_, true, String("ok-fallback"));
      return true;
    }
      recordGlobalTrace(endpointForDebug(host_, port_), legacyStatusPath, code, lastAttemptCount_, lastLatencyMs_, false, err.length() ? err : "fallback_status_failed");
      String reason = err.length() ? err : "fallback_status_failed";
      endpointSummary += String(" ") + legacyStatusPath + ": " + String("a") + String(attempt + 1) + String("=") + String(code) + ":" + reason;
      if (code) {
        lastStatusCode_ = code;
      }
    if (attempt + 1 < attempts) {
      const uint16_t delayMs = min<uint16_t>(maxDelayMs, baseDelayMs << attempt);
      delay(delayMs);
    }
  }
  if (responseStatus) {
    responseStatus->connected = false;
    responseStatus->ok = false;
    responseStatus->error = endpointSummary.length() ? endpointSummary : (err.length() ? err : String("request_failed_") + String(code));
  }
  lastError_ = err.length() ? err : responseStatus ? responseStatus->error : String("request_failed_") + String(code);
  recordGlobalTrace(endpointForDebug(host_, port_), lastPath_, lastStatusCode_, lastAttemptCount_, lastLatencyMs_, false, lastError_);
  return false;
}
