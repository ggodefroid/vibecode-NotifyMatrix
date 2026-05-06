#include "idfm_client.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <cstdlib>
#include <ctime>
#include <cstring>

static constexpr size_t kIdfmUriEncodeMaxInput = 256;

static String idfm_uri_encode_component(const char* s, size_t max_input_len = kIdfmUriEncodeMaxInput)
{
  String o;
  if (s == nullptr) {
    return o;
  }
  const size_t len = strnlen(s, max_input_len);
  for (size_t i = 0; i < len; ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
        c == '_' || c == '.' || c == '~') {
      o += static_cast<char>(c);
    } else {
      char buf[5];
      snprintf(buf, sizeof(buf), "%%%02X", (unsigned)c);
      o += buf;
    }
  }
  return o;
}

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

bool is_configured(const char* value)
{
  return value != nullptr && value[0] != '\0' && std::strcmp(value, "YOUR_PRIM_KEY") != 0;
}

int64_t days_from_civil(int y, unsigned m, unsigned d)
{
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

bool parse_int(const char* s, size_t pos, size_t len, int& out)
{
  int value = 0;
  for (size_t i = 0; i < len; i++) {
    const char c = s[pos + i];
    if (c < '0' || c > '9') {
      return false;
    }
    value = value * 10 + (c - '0');
  }
  out = value;
  return true;
}

bool parse_iso8601_epoch(const char* iso, time_t& out)
{
  if (iso == nullptr) {
    return false;
  }
  const size_t len = std::strlen(iso);
  if (len < 16) {
    return false;
  }

  int y = 0;
  int mo = 0;
  int d = 0;
  int h = 0;
  int mi = 0;
  int sec = 0;
  if (!parse_int(iso, 0, 4, y) || iso[4] != '-' || !parse_int(iso, 5, 2, mo) ||
      iso[7] != '-' || !parse_int(iso, 8, 2, d) || (iso[10] != 'T' && iso[10] != 't') ||
      !parse_int(iso, 11, 2, h) || iso[13] != ':' || !parse_int(iso, 14, 2, mi)) {
    return false;
  }

  size_t pos = 16;
  if (pos < len && iso[pos] == ':') {
    if (len < 19 || !parse_int(iso, 17, 2, sec)) {
      return false;
    }
    pos = 19;
    if (pos < len && iso[pos] == '.') {
      pos++;
      while (pos < len && iso[pos] >= '0' && iso[pos] <= '9') {
        pos++;
      }
    }
  }

  int offset_seconds = 0;
  if (pos < len) {
    const char tz = iso[pos];
    if (tz == 'Z' || tz == 'z') {
      offset_seconds = 0;
      pos++;
    } else if (tz == '+' || tz == '-') {
      int sign = (tz == '+') ? 1 : -1;
      int oh = 0;
      int om = 0;
      if (pos + 3 <= len && iso[pos + 1] >= '0' && iso[pos + 1] <= '9' &&
          iso[pos + 2] >= '0' && iso[pos + 2] <= '9') {
        if (pos + 3 < len && iso[pos + 3] == ':') {
          if (pos + 6 > len || !parse_int(iso, pos + 4, 2, om)) {
            return false;
          }
        } else if (pos + 5 <= len && iso[pos + 3] >= '0' && iso[pos + 3] <= '9' &&
                   iso[pos + 4] >= '0' && iso[pos + 4] <= '9') {
          if (!parse_int(iso, pos + 3, 2, om)) {
            return false;
          }
        } else {
          om = 0;
        }
        if (!parse_int(iso, pos + 1, 2, oh)) {
          return false;
        }
        offset_seconds = sign * (oh * 3600 + om * 60);
      } else {
        return false;
      }
    }
  }

  const int64_t days = days_from_civil(y, (unsigned)mo, (unsigned)d);
  out = (time_t)(days * 86400 + h * 3600 + mi * 60 + sec - offset_seconds);
  return true;
}

const char* first_string(JsonVariant value)
{
  if (value.is<const char*>()) {
    return value.as<const char*>();
  }
  if (value.is<JsonArray>()) {
    JsonVariant first = value[0];
    if (first["value"].is<const char*>()) {
      return first["value"].as<const char*>();
    }
  }
  if (value["value"].is<const char*>()) {
    return value["value"].as<const char*>();
  }
  return nullptr;
}

const char* siri_datetime_string(JsonVariant v)
{
  if (v.is<const char*>()) {
    return v.as<const char*>();
  }
  if (v.is<JsonObject>()) {
    JsonObject o = v.as<JsonObject>();
    if (o["value"].is<const char*>()) {
      return o["value"].as<const char*>();
    }
  }
  return nullptr;
}

static bool iso8601_to_hhmm(const char* iso, char* out, size_t out_size)
{
  if (out_size == 0) {
    return false;
  }
  out[0] = '\0';
  time_t epoch = 0;
  if (!parse_iso8601_epoch(iso, epoch)) {
    return false;
  }
  struct tm local_tm;
  localtime_r(&epoch, &local_tm);
  if (strftime(out, out_size, "%H:%M", &local_tm) == 0) {
    return false;
  }
  return true;
}

bool find_time_value(JsonObject call,
                     bool prefer_theoretical,
                     const char*& out_iso,
                     time_t& out_epoch)
{
  static const char* const kTheoreticalOrder[] = {"AimedDepartureTime",
                                                   "AimedArrivalTime",
                                                   "ExpectedDepartureTime",
                                                   "ExpectedArrivalTime",
                                                   "ActualDepartureTime",
                                                   "ActualArrivalTime"};
  static const char* const kRealtimeOrder[] = {"ExpectedDepartureTime",
                                                "ExpectedArrivalTime",
                                                "AimedDepartureTime",
                                                "AimedArrivalTime",
                                                "ActualDepartureTime",
                                                "ActualArrivalTime"};
  const char* const* order = prefer_theoretical ? kTheoreticalOrder : kRealtimeOrder;
  const size_t order_len = prefer_theoretical ? sizeof(kTheoreticalOrder) / sizeof(kTheoreticalOrder[0])
                                             : sizeof(kRealtimeOrder) / sizeof(kRealtimeOrder[0]);
  for (size_t i = 0; i < order_len; ++i) {
    const char* s = siri_datetime_string(call[order[i]]);
    if (s == nullptr || s[0] == '\0') {
      continue;
    }
    time_t epoch = 0;
    if (parse_iso8601_epoch(s, epoch)) {
      out_iso = s;
      out_epoch = epoch;
      return true;
    }
  }
  return false;
}

/// PRIM LineRef can be either a direct string or an object like {"value": "STIF:..."}.
const char* journey_line_ref_cstr(JsonObject journey)
{
  JsonVariant lr = journey["LineRef"];
  if (lr.isNull()) {
    return "";
  }
  if (lr.is<const char*>()) {
    const char* s = lr.as<const char*>();
    return s != nullptr ? s : "";
  }
  if (lr.is<JsonObject>()) {
    JsonObject o = lr.as<JsonObject>();
    if (o["value"].is<const char*>()) {
      const char* s = o["value"].as<const char*>();
      return s != nullptr ? s : "";
    }
  }
  return "";
}

const char* journey_destination_cstr(JsonObject journey)
{
  const char* s = first_string(journey["DestinationName"]);
  if (s != nullptr && s[0] != '\0') {
    return s;
  }
  s = first_string(journey["DirectionName"]);
  if (s != nullptr && s[0] != '\0') {
    return s;
  }
  s = first_string(journey["DestinationRef"]);
  if (s != nullptr && s[0] != '\0') {
    return s;
  }
  return "";
}

static bool case_insensitive_equal(char a, char b)
{
  if (a >= 'A' && a <= 'Z') {
    a = static_cast<char>(a - 'A' + 'a');
  }
  if (b >= 'A' && b <= 'Z') {
    b = static_cast<char>(b - 'A' + 'a');
  }
  return a == b;
}

static bool case_insensitive_contains(const char* haystack, const char* needle)
{
  if (haystack == nullptr || needle == nullptr) {
    return false;
  }
  if (needle[0] == '\0') {
    return true;
  }
  size_t needle_len = std::strlen(needle);
  if (needle_len == 0) {
    return true;
  }
  for (const char* h = haystack; *h != '\0'; ++h) {
    size_t i = 0;
    while (h[i] != '\0' && needle[i] != '\0' && case_insensitive_equal(h[i], needle[i])) {
      i++;
    }
    if (needle[i] == '\0') {
      return true;
    }
  }
  return false;
}

static bool destination_filter_matches(const char* dest, const char* destination_filter)
{
  if (dest == nullptr || dest[0] == '\0') {
    return false;
  }
  if (destination_filter == nullptr || destination_filter[0] == '\0') {
    return true;
  }

  if (case_insensitive_contains(dest, destination_filter)) {
    return true;
  }

  // Match common Paris Est synonyms and abbreviations.
  if (case_insensitive_contains(destination_filter, "paris") &&
      case_insensitive_contains(destination_filter, "est")) {
    if (case_insensitive_contains(dest, "gare de l'est") ||
        case_insensitive_contains(dest, "gare de l est") ||
        case_insensitive_contains(dest, "gde") ||
        case_insensitive_contains(dest, "paris est") ||
        case_insensitive_contains(dest, "paris-est")) {
      return true;
    }
  }
  return false;
}

bool label_is_all_ascii_digits(const char* s)
{
  if (s == nullptr || s[0] == '\0') {
    return false;
  }
  for (const char* p = s; *p != '\0'; p++) {
    if (*p < '0' || *p > '9') {
      return false;
    }
  }
  return true;
}

/// Display label from `.env` (for example "0628" or "2225") vs STIF LineRef.
/// Network codes are often "C" plus five digits, so "0628" also checks "C00628".
bool line_label_matches_stif_line_ref(const char* lr, const char* label)
{
  if (lr == nullptr || lr[0] == '\0' || label == nullptr || label[0] == '\0') {
    return false;
  }
  if (std::strstr(lr, label) != nullptr) {
    return true;
  }
  if (label_is_all_ascii_digits(label)) {
    const int n = std::atoi(label);
    if (n > 0 && n <= 99999) {
      char code[12];
      snprintf(code, sizeof(code), "C%05d", n);
      if (std::strstr(lr, code) != nullptr) {
        return true;
      }
    }
  }
  return false;
}

static bool should_apply_destination_filter(const char* line_label_for_pub,
                                             const char* destination_filter)
{
  return destination_filter != nullptr && destination_filter[0] != '\0' &&
         line_label_for_pub != nullptr && std::strcmp(line_label_for_pub, "P") == 0;
}

bool journey_matches_line_filter(JsonObject journey,
                                 const char* line_ref,
                                 const char* line_code_substr,
                                 const char* line_label_for_pub,
                                 const char* destination_filter)
{
  const bool has_ref = line_ref != nullptr && line_ref[0] != '\0';
  const bool has_code = line_code_substr != nullptr && line_code_substr[0] != '\0';
  const bool has_label = line_label_for_pub != nullptr && line_label_for_pub[0] != '\0';

  const char* lr = journey_line_ref_cstr(journey);

  if (has_ref && lr[0] != '\0') {
    if (std::strcmp(lr, line_ref) == 0 ||
        (std::strlen(line_ref) >= 5 && std::strstr(lr, line_ref) != nullptr)) {
      if (should_apply_destination_filter(line_label_for_pub, destination_filter)) {
        const char* dest = journey_destination_cstr(journey);
        return destination_filter_matches(dest, destination_filter);
      }
      return true;
    }
  }

  // route_id values like IDFM:C00628 usually appear as STIF:Line::C00628: in SIRI LineRef.
  if (has_code && has_label) {
    if (lr[0] == '\0' || std::strstr(lr, line_code_substr) == nullptr) {
      return false;
    }
    if (should_apply_destination_filter(line_label_for_pub, destination_filter)) {
      const char* dest = journey_destination_cstr(journey);
      return destination_filter_matches(dest, destination_filter);
    }
    // LineRef identifies the line. PublishedLineName is often missing or does not contain
    // the commercial number, so do not require it when the STIF code already matches.
    return true;
  }

  if (has_code) {
    if (lr[0] == '\0' || std::strstr(lr, line_code_substr) == nullptr) {
      return false;
    }
    if (should_apply_destination_filter(line_label_for_pub, destination_filter)) {
      const char* dest = journey_destination_cstr(journey);
      return destination_filter_matches(dest, destination_filter);
    }
    return true;
  }

  if (has_label) {
    const char* pub = first_string(journey["PublishedLineName"]);
    if (pub != nullptr && pub[0] != '\0' && std::strstr(pub, line_label_for_pub) != nullptr) {
      if (should_apply_destination_filter(line_label_for_pub, destination_filter)) {
        const char* dest = journey_destination_cstr(journey);
        return destination_filter_matches(dest, destination_filter);
      }
      return true;
    }
    if (line_label_matches_stif_line_ref(lr, line_label_for_pub)) {
      if (should_apply_destination_filter(line_label_for_pub, destination_filter)) {
        const char* dest = journey_destination_cstr(journey);
        return destination_filter_matches(dest, destination_filter);
      }
      return true;
    }
    return false;
  }

  if (destination_filter != nullptr && destination_filter[0] != '\0') {
    if (should_apply_destination_filter(line_label_for_pub, destination_filter)) {
      const char* dest = journey_destination_cstr(journey);
      return destination_filter_matches(dest, destination_filter);
    }
    return true;
  }

  return !has_ref;
}

/// One MonitoredStopVisit item. SIRI can return either an array or a single object.
void idfm_consider_one_visit(JsonObject visit,
                             const char* line_ref,
                             const char* line_code_substr,
                             const char* fallback_line_label,
                             bool prefer_theoretical,
                             time_t now,
                             int& best_minutes,
                             const char*& best_label,
                             const char*& best_departure_iso,
                             unsigned& skipped_line,
                             unsigned& skipped_bad_iso,
                             unsigned& skipped_past,
                             unsigned& kept_line)
{
  JsonObject journey = visit["MonitoredVehicleJourney"];
  if (journey.isNull()) {
    return;
  }
  if (!journey_matches_line_filter(journey,
                                     line_ref,
                                     line_code_substr,
                                     fallback_line_label,
                                     IDFM_DESTINATION_FILTER)) {
    skipped_line++;
    return;
  }
  kept_line++;

  const char* label = first_string(journey["PublishedLineName"]);
  if (label == nullptr || label[0] == '\0') {
    label = fallback_line_label;
  }

  JsonObject call = journey["MonitoredCall"];
  const char* departure = nullptr;
  time_t departure_epoch = 0;
  if (!find_time_value(call, prefer_theoretical, departure, departure_epoch)) {
    skipped_bad_iso++;
    if (skipped_bad_iso <= 2u) {
      const char* raw = siri_datetime_string(call["AimedDepartureTime"]);
      if (raw == nullptr) {
        raw = siri_datetime_string(call["AimedArrivalTime"]);
      }
      Serial.printf("[IDFM] dbg skip_bad_iso preferred dep=%.40s\n",
                    raw != nullptr ? raw : "(null)");
    }
    return;
  }

  const long delta_seconds = (long)difftime(departure_epoch, now);
  if (delta_seconds < -60) {
    skipped_past++;
    return;
  }
  int minutes = (int)((delta_seconds + 59) / 60);
  if (minutes < 0) {
    minutes = 0;
  }
  if (minutes < best_minutes) {
    best_minutes = minutes;
    best_label = label;
    best_departure_iso = departure;
  }
}

void set_error(IdfmResult& result, const char* text, int http_code = 0)
{
  result.ok = false;
  result.minutes = -1;
  result.http_code = http_code;
  copy_cstr(result.text, sizeof(result.text), "ERR");
  copy_cstr(result.error, sizeof(result.error), text);
}

/// Truncate long HTTP/JSON bodies for serial output.
void idfm_serial_trunc(const char* prefix, const char* body, size_t max_print = 220)
{
  constexpr size_t kBufCap = 512;
  if (body == nullptr) {
    Serial.printf("%s (null)\n", prefix);
    return;
  }
  if (max_print > kBufCap - 1) {
    max_print = kBufCap - 1;
  }
  const size_t n = std::strlen(body);
  if (n <= max_print) {
    Serial.printf("%s%s\n", prefix, body);
    return;
  }
  char buf[kBufCap];
  std::memcpy(buf, body, max_print);
  buf[max_print] = '\0';
  Serial.printf("%s%s... (total %u bytes)\n", prefix, buf, (unsigned)n);
}

static void idfm_serial_print_hex(const char* prefix, const char* data, size_t len)
{
  Serial.print(prefix);
  for (size_t i = 0; i < len; ++i) {
    char buf[4];
    snprintf(buf, sizeof(buf), "%02X", static_cast<unsigned char>(data[i]));
    Serial.print(buf);
    if (i + 1 < len) {
      Serial.print(' ');
    }
  }
  Serial.println();
}

/// Log that an API key exists without printing it in full.
void idfm_log_api_key_meta(const char* api_key)
{
  if (api_key == nullptr || api_key[0] == '\0') {
    Serial.println("[IDFM] apikey header: (empty)");
    return;
  }
  const size_t n = std::strlen(api_key);
  if (n <= 4u) {
    Serial.printf("[IDFM] apikey header: len=%u (very short key - check .env)\n", (unsigned)n);
    return;
  }
  Serial.printf("[IDFM] apikey header: len=%u start=%.2s...end=%.2s\n",
                (unsigned)n,
                api_key,
                api_key + (n - 2));
}

/// Print a long string, such as a URL, in chunks to avoid Serial.printf buffer issues.
void idfm_serial_print_cstr_chunks(const char* label, const char* s)
{
  Serial.print(label);
  if (s == nullptr) {
    Serial.println("(null)");
    return;
  }
  for (size_t i = 0; s[i] != '\0'; i += 120) {
    char piece[121];
    size_t j = 0;
    for (; j < 120 && s[i + j] != '\0'; j++) {
      piece[j] = s[i + j];
    }
    piece[j] = '\0';
    Serial.print(piece);
  }
  Serial.println();
}

/// PRIM sometimes returns StopMonitoringDelivery as an array, sometimes as one object.
/// A JSON filter hard-coded to [0] would then hide the whole payload.
JsonObject idfm_get_stop_monitoring_delivery(JsonVariant smd)
{
  if (smd.isNull()) {
    return JsonObject();
  }
  if (smd.is<JsonArray>()) {
    JsonArray arr = smd.as<JsonArray>();
    if (arr.size() == 0) {
      return JsonObject();
    }
    return arr[0].as<JsonObject>();
  }
  if (smd.is<JsonObject>()) {
    return smd.as<JsonObject>();
  }
  return JsonObject();
}

} // namespace

bool idfm_fetch_next_departure(const char* api_key,
                               const char* monitoring_ref,
                               const char* line_ref,
                               const char* line_code_substr,
                               const char* fallback_line_label,
                               bool prefer_theoretical,
                               IdfmResult& result)
{
  const uint32_t t_start_ms = millis();
  result = IdfmResult{};
  copy_cstr(result.line, sizeof(result.line), fallback_line_label);

  Serial.printf("[IDFM] --- request ms=%lu heap=%u epoch=%lld\n",
                (unsigned long)t_start_ms,
                (unsigned)ESP.getFreeHeap(),
                (long long)time(nullptr));

  if (!is_configured(api_key) || !is_configured(monitoring_ref)) {
    Serial.printf("[IDFM] FAIL missing_config key_ok=%d ref_ok=%d\n",
                  is_configured(api_key) ? 1 : 0,
                  is_configured(monitoring_ref) ? 1 : 0);
    set_error(result, "missing config");
    copy_cstr(result.text, sizeof(result.text), "+1h");
    return false;
  }

  if (time(nullptr) < 1700000000) {
    Serial.printf("[IDFM] FAIL time_not_synced epoch=%lld\n", (long long)time(nullptr));
    set_error(result, "time not synced");
    copy_cstr(result.text, sizeof(result.text), "+1h");
    return false;
  }

  const size_t monitoring_ref_len = monitoring_ref != nullptr ? strnlen(monitoring_ref, kIdfmUriEncodeMaxInput) : 0;
  const size_t line_ref_len = line_ref != nullptr ? strnlen(line_ref, kIdfmUriEncodeMaxInput) : 0;
  Serial.printf("[IDFM] ref=%.48s line_ref=%.32s code=%.24s label=%.16s\n",
                monitoring_ref != nullptr ? monitoring_ref : "(null)",
                line_ref != nullptr && line_ref[0] != '\0' ? line_ref : "(empty)",
                line_code_substr != nullptr && line_code_substr[0] != '\0' ? line_code_substr : "(empty)",
                fallback_line_label != nullptr ? fallback_line_label : "(null)");
  Serial.printf("[IDFM] ref len=%u line_ref len=%u\n",
                (unsigned)monitoring_ref_len,
                (unsigned)line_ref_len);
  if (line_ref_len == kIdfmUriEncodeMaxInput) {
    Serial.println(F("[IDFM] WARN line_ref appears too long or unterminated"));
    idfm_serial_print_hex("[IDFM] line_ref raw bytes: ", line_ref, 16);
  }

  String uri = "/marketplace/stop-monitoring?MonitoringRef=";
  uri += idfm_uri_encode_component(monitoring_ref);
  if (line_ref != nullptr && line_ref[0] != '\0') {
    uri += "&LineRef=";
    uri += idfm_uri_encode_component(line_ref, kIdfmUriEncodeMaxInput);
  }
  String log_url = String("https://prim.iledefrance-mobilites.fr") + uri;

  Serial.println();
  Serial.println(F("[IDFM] ========== PRIM API CALL =========="));
  Serial.println(F("[IDFM] service: GET https://prim.iledefrance-mobilites.fr/marketplace/stop-monitoring"));
  Serial.println(F("[IDFM] format: JSON (SIRI Lite), Accept header: application/json"));
  Serial.println(F("[IDFM] TLS: WiFiClientSecure + setInsecure() (no certificate pinning)"));
  Serial.printf("[IDFM] HTTPClient timeout=%u ms | heap before request=%u\n",
                15000u,
                (unsigned)ESP.getFreeHeap());
  Serial.printf("[IDFM] WiFi: status=%d (3=WL_CONNECTED) RSSI=%d dBm\n",
                (int)WiFi.status(),
                (int)WiFi.RSSI());
  idfm_log_api_key_meta(api_key);
  idfm_serial_print_cstr_chunks("[IDFM] URL (encoded MonitoringRef + LineRef): ", log_url.c_str());
  Serial.printf("[IDFM] service host=%s port=443\n", "prim.iledefrance-mobilites.fr");
  Serial.printf("[IDFM] URL length=%u chars\n", (unsigned)log_url.length());

  WiFiClientSecure secure_client;
  secure_client.setInsecure();

  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(secure_client,
                  "prim.iledefrance-mobilites.fr",
                  443,
                  uri.c_str(),
                  true)) {
    Serial.printf("[IDFM] FAIL http_begin(host, uri) WiFi_status=%d heap=%u\n",
                  (int)WiFi.status(),
                  (unsigned)ESP.getFreeHeap());
    Serial.println(F("[IDFM] ========== END CALL (http_begin failed) =========="));
    set_error(result, "http begin");
    return false;
  }

  http.addHeader("accept", "application/json");
  http.addHeader("apikey", api_key);
  Serial.println(F("[IDFM] sending GET..."));
  Serial.println(F("[IDFM] calling http.GET()"));

  const int http_code = http.GET();
  result.http_code = http_code;
  Serial.printf("[IDFM] http.GET returned %d (%s)\n",
                http_code,
                http.errorToString(http_code).c_str());
  Serial.printf("[IDFM] line response: HTTP %d (200=OK) dt=%lu ms heap=%u\n",
                http_code,
                (unsigned long)(millis() - t_start_ms),
                (unsigned)ESP.getFreeHeap());
  if (http_code != HTTP_CODE_OK) {
    const String err_body = http.getString();
    Serial.printf("[IDFM] FAIL http_status=%d heap=%u dt=%lums\n",
                  http_code,
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned long)(millis() - t_start_ms));
    idfm_serial_trunc("[IDFM] HTTP error body (start): ", err_body.c_str(), 400);
    http.end();
    Serial.println(F("[IDFM] ========== END CALL (HTTP failure) =========="));
    set_error(result, "http error", http_code);
    return false;
  }

  // Read the whole JSON body into RAM. Parsing directly from getStream() over TLS can be
  // incomplete on ESP32. Stop-monitoring responses stay small when IDFM_LINE_REF is set.
  const String response_body = http.getString();
  http.end();
  Serial.printf("[IDFM] body read: %u bytes | heap=%u | dt=%lu ms\n",
                (unsigned)response_body.length(),
                (unsigned)ESP.getFreeHeap(),
                (unsigned long)(millis() - t_start_ms));
  if (response_body.length() == 0) {
    Serial.printf("[IDFM] FAIL empty_response_body heap=%u dt=%lums\n",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned long)(millis() - t_start_ms));
    Serial.println(F("[IDFM] ========== END CALL (empty body) =========="));
    set_error(result, "empty body", http_code);
    return false;
  }

  idfm_serial_trunc("[IDFM] JSON body (start): ", response_body.c_str(), 400);

  JsonDocument filter;
  // Keep the whole delivery block. If StopMonitoringDelivery is an object, a [0] filter
  // would remove MonitoredStopVisit and make the parser see null.
  filter["Siri"]["ServiceDelivery"]["StopMonitoringDelivery"] = true;

  JsonDocument doc;
  // PRIM/SIRI JSON can exceed ArduinoJson's default nesting limit.
  constexpr uint8_t kJsonNestingLimit = 30;
  DeserializationError error = deserializeJson(
      doc,
      response_body,
      DeserializationOption::Filter(filter),
      DeserializationOption::NestingLimit(kJsonNestingLimit));
  if (error) {
    Serial.printf("[IDFM] FAIL json_deserialize code=%s ec=%u nesting_limit=%u heap=%u dt=%lums\n",
                  error.c_str(),
                  (unsigned)error.code(),
                  (unsigned)kJsonNestingLimit,
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned long)(millis() - t_start_ms));
    idfm_serial_trunc("[IDFM] body replay for parse debug: ", response_body.c_str(), 400);
    Serial.println(F("[IDFM] ========== END CALL (JSON parse failed) =========="));
    set_error(result, "json error", http_code);
    return false;
  }

  JsonObject delivery = idfm_get_stop_monitoring_delivery(
      doc["Siri"]["ServiceDelivery"]["StopMonitoringDelivery"]);
  if (delivery.isNull()) {
    Serial.printf("[IDFM] FAIL stop_monitoring_delivery missing/empty heap=%u dt=%lums\n",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned long)(millis() - t_start_ms));
    Serial.println(F("[IDFM] ========== END CALL (no JSON delivery) =========="));
    set_error(result, "no delivery", http_code);
    copy_cstr(result.text, sizeof(result.text), "+1h");
    return false;
  }
  if (delivery["ErrorCondition"].is<JsonObject>()) {
    JsonObject errInf = delivery["ErrorCondition"]["ErrorInformation"].as<JsonObject>();
    const char* msg = errInf["ErrorText"].as<const char*>();
    if (msg == nullptr || msg[0] == '\0') {
      msg = errInf["ErrorDescription"].as<const char*>();
    }
    if (msg == nullptr) {
      msg = "PRIM ErrorCondition";
    }
    Serial.printf("[IDFM] FAIL api_error_condition: %s (heap=%u)\n",
                  msg,
                  (unsigned)ESP.getFreeHeap());
    Serial.println(F("[IDFM] ========== END CALL (PRIM ErrorCondition) =========="));
    result.ok = false;
    result.http_code = http_code;
    copy_cstr(result.error, sizeof(result.error), msg);
    copy_cstr(result.text, sizeof(result.text), "+1h");
    return false;
  }

  JsonVariant msv = delivery["MonitoredStopVisit"];
  if (msv.isNull()) {
    Serial.printf("[IDFM] FAIL monitored_stop_visit missing/null heap=%u dt=%lums\n",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned long)(millis() - t_start_ms));
    Serial.println(F("[IDFM] ========== END CALL (no visits in JSON) =========="));
    set_error(result, "no visits", http_code);
    copy_cstr(result.text, sizeof(result.text), "+1h");
    return false;
  }

  Serial.println(F("[IDFM] JSON structure OK, analyzing MonitoredStopVisit / lines..."));

  const time_t now = time(nullptr);
  int best_minutes = 9999;
  const char* best_label = fallback_line_label;
  const char* best_departure_iso = nullptr;
  unsigned skipped_line = 0;
  unsigned skipped_bad_iso = 0;
  unsigned skipped_past = 0;
  unsigned kept_line = 0;
  size_t visit_count = 0;

  if (msv.is<JsonArray>()) {
    JsonArray visits = msv.as<JsonArray>();
    visit_count = visits.size();
    if (visit_count == 0) {
      Serial.printf("[IDFM] FAIL visits_empty (array size 0) heap=%u\n", (unsigned)ESP.getFreeHeap());
      Serial.println(F("[IDFM] ========== END CALL (empty visits list) =========="));
      set_error(result, "no visits", http_code);
      copy_cstr(result.text, sizeof(result.text), "+1h");
      return false;
    }
    for (JsonObject visit : visits) {
      idfm_consider_one_visit(visit,
                              line_ref,
                              line_code_substr,
                              fallback_line_label,
                              prefer_theoretical,
                              now,
                              best_minutes,
                              best_label,
                              best_departure_iso,
                              skipped_line,
                              skipped_bad_iso,
                              skipped_past,
                              kept_line);
    }
  } else if (msv.is<JsonObject>()) {
    visit_count = 1;
    idfm_consider_one_visit(msv.as<JsonObject>(),
                            line_ref,
                            line_code_substr,
                            fallback_line_label,
                            prefer_theoretical,
                            now,
                            best_minutes,
                            best_label,
                            best_departure_iso,
                            skipped_line,
                            skipped_bad_iso,
                            skipped_past,
                            kept_line);
  } else {
    Serial.printf("[IDFM] FAIL monitored_stop_visit unexpected type heap=%u\n",
                  (unsigned)ESP.getFreeHeap());
    Serial.println(F("[IDFM] ========== END CALL (unknown MonitoredStopVisit type) =========="));
    set_error(result, "no visits", http_code);
    copy_cstr(result.text, sizeof(result.text), "+1h");
    return false;
  }

  if (best_minutes == 9999) {
    Serial.printf(
        "[IDFM] FAIL no_match visits=%u line_ok=%u line_skip=%u bad_iso=%u past60s=%u "
        "epoch_now=%lld heap=%u dt=%lums\n",
        (unsigned)visit_count,
        kept_line,
        skipped_line,
        skipped_bad_iso,
        skipped_past,
        (long long)now,
        (unsigned)ESP.getFreeHeap(),
        (unsigned long)(millis() - t_start_ms));
    Serial.println(F("[IDFM] ========== END CALL (no matching visit after filters) =========="));
    set_error(result, "no match", http_code);
    copy_cstr(result.text, sizeof(result.text), "+1h");
    return false;
  }

  result.ok = true;
  result.minutes = best_minutes > 0 ? best_minutes - 1 : 0;
  copy_cstr(result.line, sizeof(result.line), best_label);
  if (prefer_theoretical && best_departure_iso != nullptr) {
    if (!iso8601_to_hhmm(best_departure_iso, result.text, sizeof(result.text))) {
      if (result.minutes == 0 || result.minutes == 1) {
        copy_cstr(result.text, sizeof(result.text), "QUAI");
      } else if (result.minutes == 2 || result.minutes == 3) {
        copy_cstr(result.text, sizeof(result.text), "PCH");
      } else {
        snprintf(result.text, sizeof(result.text), "%dm", result.minutes);
      }
    }
  } else if (result.minutes == 0 || result.minutes == 1) {
    copy_cstr(result.text, sizeof(result.text), "QUAI");
  } else if (result.minutes == 2 || result.minutes == 3) {
    copy_cstr(result.text, sizeof(result.text), "PCH");
  } else {
    snprintf(result.text, sizeof(result.text), "%dm", result.minutes);
  }
  copy_cstr(result.error, sizeof(result.error), "");
  Serial.printf("[IDFM] OK minutes=%d line=%.20s visits=%u heap=%u dt=%lums\n",
                best_minutes,
                best_label != nullptr ? best_label : "?",
                (unsigned)visit_count,
                (unsigned)ESP.getFreeHeap(),
                (unsigned long)(millis() - t_start_ms));
  Serial.printf("[IDFM] UI: bus text=\"%s\" ok=%d\n", result.text, (int)result.ok);
  Serial.println(F("[IDFM] ========== END CALL (success) =========="));
  return true;
}
