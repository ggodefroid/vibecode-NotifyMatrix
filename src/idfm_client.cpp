#include "idfm_client.h"

#include "runtime_config.h"

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

static bool find_time_value_order(JsonObject call,
                                   const char* const* keys,
                                   size_t key_count,
                                   const char*& out_iso,
                                   time_t& out_epoch)
{
  for (size_t i = 0; i < key_count; ++i) {
    const char* s = siri_datetime_string(call[keys[i]]);
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

bool find_time_value(JsonObject call,
                     bool prefer_theoretical,
                     const char*& out_iso,
                     time_t& out_epoch)
{
  static const char* const kTheoreticalOrder[] = {"AimedDepartureTime",
                                                   "AimedArrivalTime"};
  static const char* const kRealtimeOrder[] = {"ExpectedDepartureTime",
                                                "ExpectedArrivalTime",
                                                "ActualDepartureTime",
                                                "ActualArrivalTime"};
  if (prefer_theoretical) {
    if (find_time_value_order(call, kTheoreticalOrder, sizeof(kTheoreticalOrder) / sizeof(kTheoreticalOrder[0]), out_iso, out_epoch)) {
      return true;
    }
    return find_time_value_order(call, kRealtimeOrder, sizeof(kRealtimeOrder) / sizeof(kRealtimeOrder[0]), out_iso, out_epoch);
  }
  if (find_time_value_order(call, kRealtimeOrder, sizeof(kRealtimeOrder) / sizeof(kRealtimeOrder[0]), out_iso, out_epoch)) {
    return true;
  }
  return find_time_value_order(call, kTheoreticalOrder, sizeof(kTheoreticalOrder) / sizeof(kTheoreticalOrder[0]), out_iso, out_epoch);
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
        case_insensitive_contains(dest, "paris-est") ||
        case_insensitive_contains(dest, "paris gare de l") ||
        case_insensitive_contains(dest, "direction paris est") ||
        case_insensitive_contains(dest, "vers paris est")) {
      return true;
    }
    if (case_insensitive_contains(dest, "est") &&
        (case_insensitive_contains(dest, "paris") || case_insensitive_contains(dest, "gare"))) {
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

static bool label_is_bus_p(const char* label)
{
  if (label == nullptr || label[0] == '\0') {
    return false;
  }
  return case_insensitive_contains(label, "BUS P") || case_insensitive_contains(label, "BUSP");
}

static bool label_is_train_p_only(const char* label)
{
  if (label == nullptr || label[0] == '\0') {
    return false;
  }
  return std::strcmp(label, "P") == 0;
}

static bool should_apply_destination_filter(const char* line_label_for_pub,
                                             const char* line_code_substr,
                                             const char* destination_filter)
{
  if (destination_filter == nullptr || destination_filter[0] == '\0') {
    return false;
  }
  if (line_code_substr != nullptr && line_code_substr[0] != '\0') {
    if (std::strcmp(line_code_substr, "C01730") == 0 || std::strcmp(line_code_substr, "C01841") == 0) {
      return true;
    }
  }
  if (label_is_train_p_only(line_label_for_pub) || label_is_bus_p(line_label_for_pub)) {
    return true;
  }
  return false;
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
      if (should_apply_destination_filter(line_label_for_pub, line_code_substr, destination_filter)) {
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
    if (should_apply_destination_filter(line_label_for_pub, line_code_substr, destination_filter)) {
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
    if (should_apply_destination_filter(line_label_for_pub, line_code_substr, destination_filter)) {
      const char* dest = journey_destination_cstr(journey);
      return destination_filter_matches(dest, destination_filter);
    }
    return true;
  }

  if (has_label) {
    const char* pub = first_string(journey["PublishedLineName"]);
    if (pub != nullptr && pub[0] != '\0' && std::strstr(pub, line_label_for_pub) != nullptr) {
      if (should_apply_destination_filter(line_label_for_pub, line_code_substr, destination_filter)) {
        const char* dest = journey_destination_cstr(journey);
        return destination_filter_matches(dest, destination_filter);
      }
      return true;
    }
    if (line_label_matches_stif_line_ref(lr, line_label_for_pub)) {
      if (should_apply_destination_filter(line_label_for_pub, line_code_substr, destination_filter)) {
        const char* dest = journey_destination_cstr(journey);
        return destination_filter_matches(dest, destination_filter);
      }
      return true;
    }
    return false;
  }

  if (destination_filter != nullptr && destination_filter[0] != '\0') {
    if (should_apply_destination_filter(line_label_for_pub, line_code_substr, destination_filter)) {
      const char* dest = journey_destination_cstr(journey);
      return destination_filter_matches(dest, destination_filter);
    }
    return false;
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
  const char* filter_label =
      (line_code_substr != nullptr && line_code_substr[0] != '\0') ? line_code_substr : fallback_line_label;
  if (!journey_matches_line_filter(journey,
                                     line_ref,
                                     line_code_substr,
                                     filter_label,
                                     cfg_idfm_destination_filter())) {
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
  copy_cstr(result.text, sizeof(result.text), "--");
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

static bool idfm_cstr_is_printable_ascii(const char* s, size_t len)
{
  for (size_t i = 0; i < len; ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x20 || c > 0x7E) {
      return false;
    }
  }
  return true;
}

static void idfm_log_cstr_bytes_if_invalid(const char* label, const char* s, size_t len)
{
  if (s == nullptr || len == 0) {
    return;
  }
  if (len == kIdfmUriEncodeMaxInput || !idfm_cstr_is_printable_ascii(s, len)) {
    Serial.printf("[IDFM] WARN %s contains unusual bytes or is too long len=%u\n",
                  label,
                  (unsigned)len);
    idfm_serial_print_hex("[IDFM] raw bytes: ", s, len < 48 ? len : 48);
  }
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
void idfm_serial_print_cstr_chunks(const char* label, const char* s, size_t len)
{
  Serial.print(label);
  if (s == nullptr) {
    Serial.println("(null)");
    return;
  }
  for (size_t i = 0; i < len; i += 120) {
    const size_t chunk_len = (len - i) > 120 ? 120 : (len - i);
    Serial.write(reinterpret_cast<const uint8_t*>(s + i), chunk_len);
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

JsonObject idfm_get_general_message_delivery(JsonVariant gmd)
{
  if (gmd.isNull()) {
    return JsonObject();
  }
  if (gmd.is<JsonArray>()) {
    JsonArray arr = gmd.as<JsonArray>();
    if (arr.size() == 0) {
      return JsonObject();
    }
    return arr[0].as<JsonObject>();
  }
  if (gmd.is<JsonObject>()) {
    return gmd.as<JsonObject>();
  }
  return JsonObject();
}

bool info_message_still_active(JsonObject msg, time_t now)
{
  if (msg.isNull()) {
    return false;
  }

  JsonVariant vp = msg["ValidityPeriod"];
  if (vp.is<JsonObject>()) {
    JsonObject period = vp.as<JsonObject>();
    const char* end = siri_datetime_string(period["EndTime"]);
    if (end != nullptr && end[0] != '\0') {
      time_t epoch = 0;
      if (parse_iso8601_epoch(end, epoch) && epoch <= now) {
        return false;
      }
    }
    const char* start = siri_datetime_string(period["StartTime"]);
    if (start != nullptr && start[0] != '\0') {
      time_t epoch = 0;
      if (parse_iso8601_epoch(start, epoch) && epoch > now) {
        return false;
      }
    }
  }

  JsonVariant pub = msg["PublicationWindow"];
  if (pub.is<JsonObject>()) {
    JsonObject window = pub.as<JsonObject>();
    const char* end = siri_datetime_string(window["EndTime"]);
    if (end != nullptr && end[0] != '\0') {
      time_t epoch = 0;
      if (parse_iso8601_epoch(end, epoch) && epoch <= now) {
        return false;
      }
    }
  }

  const char* valid_until = siri_datetime_string(msg["ValidUntil"]);
  if (valid_until != nullptr && valid_until[0] != '\0') {
    time_t epoch = 0;
    if (parse_iso8601_epoch(valid_until, epoch) && epoch <= now) {
      return false;
    }
  }

  return true;
}

unsigned count_active_info_messages(JsonVariant info_msg, time_t now)
{
  if (info_msg.isNull()) {
    return 0;
  }
  unsigned count = 0;
  if (info_msg.is<JsonArray>()) {
    for (JsonObject msg : info_msg.as<JsonArray>()) {
      if (info_message_still_active(msg, now)) {
        count++;
      }
    }
    return count;
  }
  if (info_msg.is<JsonObject>()) {
    return info_message_still_active(info_msg.as<JsonObject>(), now) ? 1u : 0u;
  }
  return 0;
}

struct DisruptionCacheEntry {
  char line_ref[kIdfmUriEncodeMaxInput + 1] = "";
  bool disrupted = false;
  uint32_t cached_at_ms = 0;
  bool valid = false;
};

DisruptionCacheEntry g_disruption_cache;

static constexpr const char* kPrimHost = "prim.iledefrance-mobilites.fr";
static constexpr uint16_t kPrimPort = 443;
static constexpr uint32_t kPrimHttpTimeoutMs = 15000u;

/// GET HTTPS PRIM — même schéma que le projet « dossier 1 » (host + port + path).
static bool idfm_prim_http_get_body(const char* api_key,
                                    const String& uri,
                                    String& response_body,
                                    int& http_code_out)
{
  http_code_out = 0;
  response_body = "";
  if (!is_configured(api_key)) {
    return false;
  }

  WiFiClientSecure secure_client;
  secure_client.setInsecure();

  HTTPClient http;
  http.setTimeout(kPrimHttpTimeoutMs);
  if (!http.begin(secure_client, kPrimHost, kPrimPort, uri.c_str(), true)) {
    Serial.printf("[IDFM] FAIL http_begin(host, uri) WiFi_status=%d heap=%u\n",
                  (int)WiFi.status(),
                  (unsigned)ESP.getFreeHeap());
    return false;
  }

  http.addHeader("accept", "application/json");
  http.addHeader("apikey", api_key);
  http_code_out = http.GET();
  yield();

  if (http_code_out != HTTP_CODE_OK) {
    const String err_body = http.getString();
    Serial.printf("[IDFM] FAIL http_status=%d (%s) heap=%u\n",
                  http_code_out,
                  HTTPClient::errorToString(http_code_out).c_str(),
                  (unsigned)ESP.getFreeHeap());
    if (err_body.length() > 0) {
      idfm_serial_trunc("[IDFM] HTTP error body: ", err_body.c_str(), 400);
    }
    http.end();
    return false;
  }

  response_body = http.getString();
  http.end();
  yield();
  return response_body.length() > 0;
}

bool prim_http_get_json(const char* api_key, const String& uri, JsonDocument& doc, int& http_code_out)
{
  String response_body;
  if (!idfm_prim_http_get_body(api_key, uri, response_body, http_code_out)) {
    return false;
  }

  constexpr uint8_t kJsonNestingLimit = 30;
  const DeserializationError error = deserializeJson(
      doc, response_body, DeserializationOption::NestingLimit(kJsonNestingLimit));
  return !error;
}

bool idfm_line_has_disruption_impl(const char* api_key, const char* line_ref, bool& out_disrupted)
{
  out_disrupted = false;

  if (!is_configured(api_key) || line_ref == nullptr || line_ref[0] == '\0') {
    return false;
  }

  if (time(nullptr) < 1700000000) {
    return false;
  }

  const uint32_t now_ms = millis();
  if (g_disruption_cache.valid && std::strcmp(g_disruption_cache.line_ref, line_ref) == 0 &&
      now_ms - g_disruption_cache.cached_at_ms < (uint32_t)IDFM_DISRUPTION_CACHE_MS) {
    out_disrupted = g_disruption_cache.disrupted;
    return true;
  }

  String uri = "/marketplace/general-message?LineRef=";
  uri += idfm_uri_encode_component(line_ref);
  uri += "&InfoChannelRef=Perturbation";

#if IDFM_LOG_REDUCE == 0
  Serial.println(F("[IDFM] GET general-message (Perturbation)"));
  idfm_serial_print_cstr_chunks("[IDFM] URL: ", (String("https://prim.iledefrance-mobilites.fr") + uri).c_str(),
                                uri.length() + 40);
#endif

  JsonDocument doc;
  int http_code = 0;
  if (!prim_http_get_json(api_key, uri, doc, http_code)) {
#if IDFM_LOG_REDUCE == 0
    Serial.printf("[IDFM] disruption check failed http=%d\n", http_code);
#endif
    return false;
  }

  JsonObject delivery = idfm_get_general_message_delivery(
      doc["Siri"]["ServiceDelivery"]["GeneralMessageDelivery"]);
  if (delivery.isNull()) {
    out_disrupted = false;
  } else if (delivery["ErrorCondition"].is<JsonObject>()) {
    out_disrupted = false;
  } else {
    const time_t now = time(nullptr);
    const unsigned active = count_active_info_messages(delivery["InfoMessage"], now);
    out_disrupted = active > 0;
  }

  copy_cstr(g_disruption_cache.line_ref, sizeof(g_disruption_cache.line_ref), line_ref);
  g_disruption_cache.disrupted = out_disrupted;
  g_disruption_cache.cached_at_ms = now_ms;
  g_disruption_cache.valid = true;

  Serial.printf("[IDFM] disruption line_ref=%.24s active=%d\n",
                line_ref,
                out_disrupted ? 1 : 0);
  return true;
}

} // namespace

void idfm_prim_invalidate_dns()
{
}

bool idfm_line_has_disruption(const char* api_key, const char* line_ref, bool& out_disrupted)
{
  return idfm_line_has_disruption_impl(api_key, line_ref, out_disrupted);
}

bool idfm_disruption_from_cache(const char* line_ref, bool& out_disrupted)
{
  out_disrupted = false;
  if (line_ref == nullptr || line_ref[0] == '\0') {
    return false;
  }
  const uint32_t now_ms = millis();
  if (g_disruption_cache.valid && std::strcmp(g_disruption_cache.line_ref, line_ref) == 0 &&
      now_ms - g_disruption_cache.cached_at_ms < (uint32_t)IDFM_DISRUPTION_CACHE_MS) {
    out_disrupted = g_disruption_cache.disrupted;
    return true;
  }
  return false;
}

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
    copy_cstr(result.text, sizeof(result.text), "--");
    return false;
  }

  if (time(nullptr) < 1700000000) {
    Serial.printf("[IDFM] FAIL time_not_synced epoch=%lld\n", (long long)time(nullptr));
    set_error(result, "time not synced");
    copy_cstr(result.text, sizeof(result.text), "--");
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
  idfm_log_cstr_bytes_if_invalid("monitoring_ref", monitoring_ref, monitoring_ref_len);
  idfm_log_cstr_bytes_if_invalid("line_ref", line_ref, line_ref_len);

  const bool want_line_in_url = line_ref != nullptr && line_ref[0] != '\0';
  const uint8_t pass_count = want_line_in_url ? 2u : 1u;
  String response_body;
  int http_code = 0;
  JsonDocument doc;
  JsonObject delivery;
  bool parsed_ok = false;

  const time_t now = time(nullptr);
  int best_minutes = 9999;
  const char* best_label = fallback_line_label;
  const char* best_departure_iso = nullptr;
  unsigned skipped_line = 0;
  unsigned skipped_bad_iso = 0;
  unsigned skipped_past = 0;
  unsigned kept_line = 0;
  size_t visit_count = 0;

  for (uint8_t pass = 0; pass < pass_count; ++pass) {
    const bool use_line_in_url = want_line_in_url && pass == 0;

    String uri = "/marketplace/stop-monitoring?MonitoringRef=";
    uri += idfm_uri_encode_component(monitoring_ref);
    if (use_line_in_url) {
      uri += "&LineRef=";
      uri += idfm_uri_encode_component(line_ref, kIdfmUriEncodeMaxInput);
    }
    const String log_url = String("https://prim.iledefrance-mobilites.fr") + uri;

    Serial.println();
    Serial.println(F("[IDFM] ========== PRIM API CALL =========="));
    Serial.printf("[IDFM] pass=%u line_in_url=%d\n",
                  (unsigned)(pass + 1),
                  use_line_in_url ? 1 : 0);
    idfm_log_api_key_meta(api_key);
    idfm_serial_print_cstr_chunks("[IDFM] URL: ", log_url.c_str(), log_url.length());

    http_code = 0;
    if (idfm_prim_http_get_body(api_key, uri, response_body, http_code)) {
      result.http_code = http_code;
    } else {
      result.http_code = http_code;
      set_error(result, "http error", http_code);
      return false;
    }

    if (response_body.length() == 0) {
      set_error(result, "empty body", http_code);
      return false;
    }

    doc.clear();
    JsonDocument filter;
    filter["Siri"]["ServiceDelivery"]["StopMonitoringDelivery"] = true;
    constexpr uint8_t kJsonNestingLimit = 30;
    const DeserializationError error = deserializeJson(
        doc,
        response_body,
        DeserializationOption::Filter(filter),
        DeserializationOption::NestingLimit(kJsonNestingLimit));
    if (error) {
      set_error(result, "json error", http_code);
      return false;
    }

    delivery = idfm_get_stop_monitoring_delivery(doc["Siri"]["ServiceDelivery"]["StopMonitoringDelivery"]);
    if (delivery.isNull()) {
      set_error(result, "no delivery", http_code);
      copy_cstr(result.text, sizeof(result.text), "--");
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
      Serial.printf("[IDFM] api_error_condition: %s\n", msg);

      if (use_line_in_url && msg != nullptr && std::strstr(msg, "couple") != nullptr) {
        Serial.println(F("[IDFM] retry without LineRef (invalid couple)"));
        continue;
      }

      result.ok = false;
      copy_cstr(result.error, sizeof(result.error), msg);
      copy_cstr(result.text, sizeof(result.text), "--");
      return false;
    }

    Serial.println(F("[IDFM] JSON structure OK, analyzing MonitoredStopVisit / lines..."));

    best_minutes = 9999;
    best_label = fallback_line_label;
    best_departure_iso = nullptr;
    skipped_line = 0;
    skipped_bad_iso = 0;
    skipped_past = 0;
    kept_line = 0;
    visit_count = 0;

    JsonVariant msv = delivery["MonitoredStopVisit"];
    if (msv.isNull()) {
      if (use_line_in_url && pass + 1 < pass_count) {
        Serial.println(F("[IDFM] retry without LineRef (no MonitoredStopVisit)"));
        continue;
      }
      set_error(result, "no visits", http_code);
      copy_cstr(result.text, sizeof(result.text), "--");
      return false;
    }

    if (msv.is<JsonArray>()) {
      JsonArray visits = msv.as<JsonArray>();
      visit_count = visits.size();
      if (visit_count == 0) {
        if (use_line_in_url && pass + 1 < pass_count) {
          Serial.println(F("[IDFM] retry without LineRef (visits_empty)"));
          continue;
        }
        set_error(result, "no visits", http_code);
        copy_cstr(result.text, sizeof(result.text), "--");
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
      set_error(result, "no visits", http_code);
      copy_cstr(result.text, sizeof(result.text), "--");
      return false;
    }

    if (best_minutes == 9999) {
      if (use_line_in_url && pass + 1 < pass_count) {
        Serial.printf("[IDFM] retry without LineRef (no_match visits=%u kept=%u skip=%u)\n",
                      (unsigned)visit_count,
                      (unsigned)kept_line,
                      (unsigned)skipped_line);
        continue;
      }
    } else {
      parsed_ok = true;
      break;
    }
  }

  if (!parsed_ok || best_minutes == 9999) {
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
    copy_cstr(result.text, sizeof(result.text), "--");
    return false;
  }

  result.ok = true;
  result.minutes = best_minutes > 0 ? best_minutes - 1 : 0;
  copy_cstr(result.line, sizeof(result.line), best_label);
  if (prefer_theoretical && best_departure_iso != nullptr &&
      iso8601_to_hhmm(best_departure_iso, result.text, sizeof(result.text))) {
    // Horaire théorique affiché tel quel (HH:MM).
  } else {
    cfg_format_bus_eta_text(result.minutes,
                            prefer_theoretical,
                            best_departure_iso,
                            result.text,
                            sizeof(result.text));
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

