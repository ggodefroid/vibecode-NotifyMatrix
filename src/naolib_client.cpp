#include "naolib_client.h"

#include "runtime_config.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <cctype>
#include <cstring>

namespace {

void copy_cstr(char* dst, size_t dst_size, const char* src)
{
  if (dst_size == 0) {
    return;
  }
  if (src == nullptr) {
    src = "";
  }
  std::strncpy(dst, src, dst_size - 1);
  dst[dst_size - 1] = '\0';
}

bool line_codes_match(const char* a, const char* b)
{
  if (a == nullptr || b == nullptr || a[0] == '\0' || b[0] == '\0') {
    return true;
  }
  while (*a == ' ' || *a == '\t') {
    ++a;
  }
  while (*b == ' ' || *b == '\t') {
    ++b;
  }
  for (; *a && *b; ++a, ++b) {
    const char ca = (char)std::tolower((unsigned char)*a);
    const char cb = (char)std::tolower((unsigned char)*b);
    if (ca != cb) {
      return false;
    }
  }
  return *a == '\0' && *b == '\0';
}

int parse_temps_minutes(const char* temps)
{
  if (temps == nullptr || temps[0] == '\0') {
    return -1;
  }
  if (std::strstr(temps, "proche") != nullptr) {
    return 0;
  }
  int value = 0;
  bool any = false;
  for (const char* p = temps; *p != '\0'; ++p) {
    if (*p >= '0' && *p <= '9') {
      value = value * 10 + (*p - '0');
      any = true;
    }
  }
  return any ? value : -1;
}

bool http_get_json(const char* host, uint16_t port, const String& uri, JsonDocument& doc, int& http_code)
{
  http_code = 0;
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(12000);
  if (!http.begin(client, host, port, uri.c_str())) {
    return false;
  }
  http.addHeader("Accept", "application/json");
  const int code = http.GET();
  http_code = code;
  yield();
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }
  const String body = http.getString();
  http.end();
  yield();
  if (body.length() == 0) {
    return false;
  }
  return deserializeJson(doc, body) == DeserializationError::Ok;
}

bool fetch_passages(const char* stop_code, const char* line_code, JsonArray& out_arr, int& http_code)
{
  out_arr = JsonArray();
  if (stop_code == nullptr || stop_code[0] == '\0') {
    return false;
  }

  String uri = "/ewp/tempsattentelieu.json/";
  uri += stop_code;
  uri += "/6";
  if (line_code != nullptr && line_code[0] != '\0') {
    uri += "/";
    uri += line_code;
  }

  JsonDocument doc;
  if (!http_get_json("open.tan.fr", 80, uri, doc, http_code)) {
    return false;
  }
  if (!doc.is<JsonArray>()) {
    return false;
  }
  out_arr = doc.as<JsonArray>();
  return true;
}

} // namespace

bool naolib_fetch_next_departure(const char* stop_code,
                                 const char* line_code,
                                 const char* fallback_line_label,
                                 IdfmResult& result)
{
  result.ok = false;
  result.minutes = -1;
  result.disrupted = false;
  copy_cstr(result.error, sizeof(result.error), "naolib");
  copy_cstr(result.text, sizeof(result.text), "--");

  if (stop_code == nullptr || stop_code[0] == '\0') {
    copy_cstr(result.error, sizeof(result.error), "no stop");
    return false;
  }

  JsonArray passages;
  int http_code = 0;
  if (!fetch_passages(stop_code, line_code, passages, http_code)) {
    copy_cstr(result.error, sizeof(result.error), "http");
    return false;
  }

  int best_minutes = 9999;
  bool found = false;
  bool disrupted = false;
  const char* best_line = nullptr;

  for (JsonObject item : passages) {
    const char* num = item["ligne"]["numLigne"].as<const char*>();
    if (line_code != nullptr && line_code[0] != '\0' && num != nullptr &&
        !line_codes_match(num, line_code)) {
      continue;
    }
    if (item["infotrafic"] | false) {
      disrupted = true;
    }
    const int mins = parse_temps_minutes(item["temps"].as<const char*>());
    if (mins < 0) {
      continue;
    }
    if (mins < best_minutes) {
      best_minutes = mins;
      best_line = num;
      found = true;
    }
  }

  if (!found) {
    copy_cstr(result.error, sizeof(result.error), "no match");
    return false;
  }

  if (fallback_line_label != nullptr && fallback_line_label[0] != '\0') {
    copy_cstr(result.line, sizeof(result.line), fallback_line_label);
  } else if (best_line != nullptr) {
    copy_cstr(result.line, sizeof(result.line), best_line);
  }

  result.minutes = best_minutes > 0 ? best_minutes - 1 : 0;
  cfg_format_bus_eta_text(result.minutes, false, nullptr, result.text, sizeof(result.text));
  result.ok = true;
  result.disrupted = disrupted;
  result.http_code = http_code;
  copy_cstr(result.error, sizeof(result.error), "");
  return true;
}

bool naolib_line_has_disruption(const char* stop_code, const char* line_code, bool& out_disrupted)
{
  out_disrupted = false;
  JsonArray passages;
  int http_code = 0;
  if (!fetch_passages(stop_code, line_code, passages, http_code)) {
    return false;
  }
  for (JsonObject item : passages) {
    if (line_code != nullptr && line_code[0] != '\0') {
      const char* num = item["ligne"]["numLigne"].as<const char*>();
      if (num != nullptr && !line_codes_match(num, line_code)) {
        continue;
      }
    }
    if (item["infotrafic"] | false) {
      out_disrupted = true;
      return true;
    }
  }
  return true;
}
