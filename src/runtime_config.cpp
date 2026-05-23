#include "runtime_config.h"

#include "app_config.h"
#include "idfm_carousel.h"
#include "transit_provider.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FFat.h>
#include <Preferences.h>
#include <cctype>
#include <cstring>
#include <memory>

namespace {

constexpr char kPrefsNamespace[] = "notifymx";
constexpr char kPrefsKeyConfigured[] = "configured";
constexpr char kPrefsKeyJson[] = "json";
constexpr char kFatConfigPath[] = "/notifymx.json";
constexpr size_t kMaxStoredJsonBytes = 12000;

RuntimeConfig g_cfg;
bool g_ffat_mounted = false;

void copy_field(char* dst, size_t dst_size, const char* src)
{
  if (dst_size == 0) {
    return;
  }
  if (src == nullptr) {
    dst[0] = '\0';
    return;
  }
  std::strncpy(dst, src, dst_size - 1);
  dst[dst_size - 1] = '\0';
}

const char* json_cstr(JsonVariantConst v, const char* fallback = "")
{
  if (v.is<const char*>()) {
    const char* s = v.as<const char*>();
    return (s != nullptr) ? s : fallback;
  }
  return fallback;
}

bool mount_ffat(bool format_on_fail)
{
  if (g_ffat_mounted) {
    return true;
  }
  if (FFat.begin(format_on_fail)) {
    g_ffat_mounted = true;
    Serial.println(F("[cfg] FFat mounted"));
    return true;
  }
  Serial.println(F("[cfg] FFat mount failed"));
  return false;
}

bool parse_hex_color(JsonVariantConst v, uint8_t& r, uint8_t& g, uint8_t& b)
{
  const char* s = json_cstr(v);
  if (s[0] != '#') {
    return false;
  }
  const size_t len = std::strlen(s);
  unsigned long hex = 0;
  if (len == 7) {
    hex = std::strtoul(s + 1, nullptr, 16);
    r = (uint8_t)((hex >> 16) & 0xFF);
    g = (uint8_t)((hex >> 8) & 0xFF);
    b = (uint8_t)(hex & 0xFF);
    return true;
  }
  if (len == 4) {
    hex = std::strtoul(s + 1, nullptr, 16);
    r = (uint8_t)(((hex >> 8) & 0xF) * 17);
    g = (uint8_t)(((hex >> 4) & 0xF) * 17);
    b = (uint8_t)((hex & 0xF) * 17);
    return true;
  }
  return false;
}

void apply_display_defaults(RuntimeConfig& cfg)
{
  if (cfg.boot_splash_text[0] == '\0') {
    copy_field(cfg.boot_splash_text, sizeof(cfg.boot_splash_text), "TweakFR");
  }
  cfg.boot_selftest_ms = (uint16_t)HUB75_SELFTEST_STEP_MS;
  cfg.brightness_normal = (uint8_t)DISPLAY_BRIGHTNESS_NORMAL;
  cfg.brightness_dim = (uint8_t)DISPLAY_BRIGHTNESS_DIM;
  cfg.dim_hour_start = 20;
  cfg.dim_hour_end = 7;
  cfg.bus_eta_blink_ms = (uint16_t)BUS_ETA_BLINK_MS;
  cfg.carousel_hold_ms = (uint16_t)IDFM_HOLD_AFTER_PREFETCH_MS;
  cfg.carousel_hold_no_info_ms = (uint16_t)IDFM_HOLD_NO_INFO_MS;
}

uint8_t clamp_u8(int v, uint8_t lo, uint8_t hi)
{
  if (v < (int)lo) {
    return lo;
  }
  if (v > (int)hi) {
    return hi;
  }
  return (uint8_t)v;
}

uint16_t clamp_u16(int v, uint16_t lo, uint16_t hi)
{
  if (v < (int)lo) {
    return lo;
  }
  if (v > (int)hi) {
    return hi;
  }
  return (uint16_t)v;
}

uint8_t parse_eta_display_mode(JsonVariantConst v)
{
  if (v.is<int>() || v.is<long>()) {
    const int m = v.as<int>();
    if (m == 1) {
      return kEtaDisplayMinutesOnly;
    }
    if (m == 2) {
      return kEtaDisplayLabelsOnly;
    }
    return kEtaDisplayAuto;
  }
  const char* s = json_cstr(v);
  if (std::strcmp(s, "minutes") == 0) {
    return kEtaDisplayMinutesOnly;
  }
  if (std::strcmp(s, "labels") == 0) {
    return kEtaDisplayLabelsOnly;
  }
  return kEtaDisplayAuto;
}

const char* eta_display_mode_to_string(uint8_t mode)
{
  if (mode == kEtaDisplayMinutesOnly) {
    return "minutes";
  }
  if (mode == kEtaDisplayLabelsOnly) {
    return "labels";
  }
  return "auto";
}

void apply_display_json(JsonObjectConst root, RuntimeConfig& cfg)
{
  copy_field(cfg.setup_line1, sizeof(cfg.setup_line1), json_cstr(root["setup_line1"]));
  copy_field(cfg.setup_line2, sizeof(cfg.setup_line2), json_cstr(root["setup_line2"]));
  copy_field(cfg.setup_line3, sizeof(cfg.setup_line3), json_cstr(root["setup_line3"]));
  {
    const char* splash = json_cstr(root["boot_splash_text"], "TweakFR");
    if (splash[0] != '\0') {
      copy_field(cfg.boot_splash_text, sizeof(cfg.boot_splash_text), splash);
    } else if (cfg.boot_splash_text[0] == '\0') {
      copy_field(cfg.boot_splash_text, sizeof(cfg.boot_splash_text), "TweakFR");
    }
  }
  if (!root["boot_selftest_ms"].isNull()) {
    const uint16_t ms = (uint16_t)(root["boot_selftest_ms"].as<uint16_t>());
    cfg.boot_selftest_ms = (ms > 0) ? ms : (uint16_t)HUB75_SELFTEST_STEP_MS;
  } else if (cfg.boot_selftest_ms == 0) {
    cfg.boot_selftest_ms = (uint16_t)HUB75_SELFTEST_STEP_MS;
  }

  uint8_t r = cfg.color_time_r;
  uint8_t g = cfg.color_time_g;
  uint8_t b = cfg.color_time_b;
  if (parse_hex_color(root["color_time"], r, g, b)) {
    cfg.color_time_r = r;
    cfg.color_time_g = g;
    cfg.color_time_b = b;
  }
  if (parse_hex_color(root["color_eta_green"], r, g, b)) {
    cfg.color_eta_green_r = r;
    cfg.color_eta_green_g = g;
    cfg.color_eta_green_b = b;
  }
  if (parse_hex_color(root["color_eta_orange"], r, g, b)) {
    cfg.color_eta_orange_r = r;
    cfg.color_eta_orange_g = g;
    cfg.color_eta_orange_b = b;
  }
  if (parse_hex_color(root["color_eta_red"], r, g, b)) {
    cfg.color_eta_red_r = r;
    cfg.color_eta_red_g = g;
    cfg.color_eta_red_b = b;
  }
  if (parse_hex_color(root["color_eta_noinfo"], r, g, b)) {
    cfg.color_eta_noinfo_r = r;
    cfg.color_eta_noinfo_g = g;
    cfg.color_eta_noinfo_b = b;
  }
  if (parse_hex_color(root["color_bus_line"], r, g, b)) {
    cfg.color_bus_line_r = r;
    cfg.color_bus_line_g = g;
    cfg.color_bus_line_b = b;
  }
  if (parse_hex_color(root["color_blink_on"], r, g, b)) {
    cfg.color_blink_on_r = r;
    cfg.color_blink_on_g = g;
    cfg.color_blink_on_b = b;
  }
  if (parse_hex_color(root["color_blink_off"], r, g, b)) {
    cfg.color_blink_off_r = r;
    cfg.color_blink_off_g = g;
    cfg.color_blink_off_b = b;
  }

  if (!root["bus_eta_blink_ms"].isNull()) {
    cfg.bus_eta_blink_ms = clamp_u16((int)(root["bus_eta_blink_ms"] | 0), 50, 2000);
  }
  if (!root["carousel_hold_ms"].isNull()) {
    const int hold = (int)(root["carousel_hold_ms"] | 0);
    if (hold >= 1000) {
      cfg.carousel_hold_ms = (uint16_t)(hold > 60000 ? 60000 : hold);
    }
  }
  if (!root["carousel_hold_no_info_ms"].isNull()) {
    const int hold = (int)(root["carousel_hold_no_info_ms"] | 0);
    if (hold >= 1000) {
      cfg.carousel_hold_no_info_ms = (uint16_t)(hold > 60000 ? 60000 : hold);
    }
  }
  if (!root["hide_carousel_no_info"].isNull()) {
    cfg.hide_carousel_no_info = root["hide_carousel_no_info"].as<bool>();
  } else if (!root["hide_bus_p_no_info"].isNull()) {
    cfg.hide_carousel_no_info = root["hide_bus_p_no_info"].as<bool>();
  }

  const uint8_t th_green = (uint8_t)(root["eta_threshold_green"] | 0);
  if (th_green > 0) {
    cfg.eta_threshold_green = th_green;
  }
  const uint8_t th_orange = (uint8_t)(root["eta_threshold_orange"] | 0);
  if (th_orange > 0) {
    cfg.eta_threshold_orange = th_orange;
  }

  if (!root["eta_display_mode"].isNull()) {
    cfg.eta_display_mode = parse_eta_display_mode(root["eta_display_mode"]);
  }
  if (!root["minutes_quai_max"].isNull()) {
    cfg.minutes_quai_max = (uint8_t)(root["minutes_quai_max"] | 0);
  }
  if (!root["minutes_pch_max"].isNull()) {
    cfg.minutes_pch_max = (uint8_t)(root["minutes_pch_max"] | 0);
  }
  copy_field(cfg.eta_label_quai, sizeof(cfg.eta_label_quai), json_cstr(root["eta_label_quai"], "QUAI"));
  copy_field(cfg.eta_label_pch, sizeof(cfg.eta_label_pch), json_cstr(root["eta_label_pch"], "PCH"));
  cfg.eta_quai_show_minutes = root["eta_quai_show_minutes"] | false;
  cfg.eta_pch_show_minutes = root["eta_pch_show_minutes"] | false;

  if (!root["brightness_normal"].isNull()) {
    cfg.brightness_normal = clamp_u8((int)(root["brightness_normal"] | 0), 0, 255);
  }
  if (!root["brightness_dim"].isNull()) {
    cfg.brightness_dim = clamp_u8((int)(root["brightness_dim"] | 0), 0, 255);
  }
  if (!root["dim_hour_start"].isNull()) {
    cfg.dim_hour_start = clamp_u8((int)(root["dim_hour_start"] | 0), 0, 23);
  }
  if (!root["dim_hour_end"].isNull()) {
    cfg.dim_hour_end = clamp_u8((int)(root["dim_hour_end"] | 0), 0, 23);
  }
}

void apply_compile_time_defaults(RuntimeConfig& cfg)
{
  copy_field(cfg.wifi_ssid, sizeof(cfg.wifi_ssid), WIFI_SSID);
  copy_field(cfg.wifi_password, sizeof(cfg.wifi_password), WIFI_PASSWORD);
  copy_field(cfg.mqtt_host, sizeof(cfg.mqtt_host), MQTT_HOST);
  cfg.mqtt_port = (uint16_t)MQTT_PORT;
  copy_field(cfg.mqtt_user, sizeof(cfg.mqtt_user), MQTT_USER);
  copy_field(cfg.mqtt_password, sizeof(cfg.mqtt_password), MQTT_PASSWORD);
  copy_field(cfg.mqtt_topic_notify, sizeof(cfg.mqtt_topic_notify), MQTT_TOPIC_NOTIFY);
  copy_field(cfg.idfm_api_key, sizeof(cfg.idfm_api_key), IDFM_API_KEY);
  copy_field(cfg.idfm_destination_filter, sizeof(cfg.idfm_destination_filter), IDFM_DESTINATION_FILTER);
  copy_field(cfg.idfm_monitoring_ref, sizeof(cfg.idfm_monitoring_ref), IDFM_MONITORING_REF);
  copy_field(cfg.idfm_line_ref, sizeof(cfg.idfm_line_ref), IDFM_LINE_REF);
  copy_field(cfg.idfm_line_code, sizeof(cfg.idfm_line_code), IDFM_LINE_CODE);
  copy_field(cfg.idfm_line_label, sizeof(cfg.idfm_line_label), IDFM_LINE_LABEL);
  cfg.idfm_prefer_theoretical = IDFM_PREFER_THEORETICAL != 0;

  cfg.slot_count = 0;
  if (kIdfmCarouselCount > 0) {
    cfg.slot_count = (uint8_t)(kIdfmCarouselCount > 16 ? 16 : kIdfmCarouselCount);
    for (uint8_t i = 0; i < cfg.slot_count; ++i) {
      const IdfmCarouselSlot& src = kIdfmCarouselSlots[i];
      RuntimeIdfmSlot& s = cfg.slots[i];
      copy_field(s.monitoring_ref, sizeof(s.monitoring_ref), src.monitoring_ref);
      copy_field(s.line_ref, sizeof(s.line_ref), src.line_ref);
      copy_field(s.line_code, sizeof(s.line_code), src.line_code);
      copy_field(s.label, sizeof(s.label), src.label);
      s.prefer_theoretical = src.prefer_theoretical;
    }
  }
  apply_display_defaults(cfg);
}

bool apply_json_object(JsonObjectConst root, RuntimeConfig& cfg)
{
  if (root.isNull()) {
    return false;
  }

  copy_field(cfg.wifi_ssid, sizeof(cfg.wifi_ssid), json_cstr(root["wifi_ssid"]));
  copy_field(cfg.wifi_password, sizeof(cfg.wifi_password), json_cstr(root["wifi_password"]));
  copy_field(cfg.mqtt_host, sizeof(cfg.mqtt_host), json_cstr(root["mqtt_host"]));
  cfg.mqtt_port = (uint16_t)(root["mqtt_port"] | 1883);
  copy_field(cfg.mqtt_user, sizeof(cfg.mqtt_user), json_cstr(root["mqtt_user"]));
  copy_field(cfg.mqtt_password, sizeof(cfg.mqtt_password), json_cstr(root["mqtt_password"]));
  copy_field(cfg.mqtt_topic_notify,
            sizeof(cfg.mqtt_topic_notify),
            json_cstr(root["mqtt_topic_notify"], "notifymatrix/notify"));
  copy_field(cfg.idfm_api_key, sizeof(cfg.idfm_api_key), json_cstr(root["idfm_api_key"]));
  copy_field(cfg.transit_api_base,
            sizeof(cfg.transit_api_base),
            json_cstr(root["transit_api_base"], "https://app.mecatran.com"));
  copy_field(cfg.idfm_destination_filter,
            sizeof(cfg.idfm_destination_filter),
            json_cstr(root["idfm_destination_filter"]));
  copy_field(cfg.idfm_monitoring_ref, sizeof(cfg.idfm_monitoring_ref), json_cstr(root["idfm_monitoring_ref"]));
  copy_field(cfg.idfm_line_ref, sizeof(cfg.idfm_line_ref), json_cstr(root["idfm_line_ref"]));
  copy_field(cfg.idfm_line_code, sizeof(cfg.idfm_line_code), json_cstr(root["idfm_line_code"]));
  copy_field(cfg.idfm_line_label, sizeof(cfg.idfm_line_label), json_cstr(root["idfm_line_label"], "2225"));
  copy_field(cfg.idfm_provider, sizeof(cfg.idfm_provider), json_cstr(root["idfm_provider"], "idfm"));
  copy_field(cfg.idfm_feed_key, sizeof(cfg.idfm_feed_key), json_cstr(root["idfm_feed_key"]));
  cfg.idfm_prefer_theoretical = root["idfm_prefer_theoretical"] | false;

  cfg.slot_count = 0;
  JsonArrayConst slots = root["slots"].as<JsonArrayConst>();
  if (!slots.isNull()) {
    for (JsonObjectConst s : slots) {
      if (cfg.slot_count >= 16) {
        break;
      }
      RuntimeIdfmSlot& slot = cfg.slots[cfg.slot_count++];
      copy_field(slot.monitoring_ref, sizeof(slot.monitoring_ref), json_cstr(s["monitoring_ref"]));
      copy_field(slot.line_ref, sizeof(slot.line_ref), json_cstr(s["line_ref"]));
      copy_field(slot.line_code, sizeof(slot.line_code), json_cstr(s["line_code"]));
      copy_field(slot.label, sizeof(slot.label), json_cstr(s["label"]));
      copy_field(slot.provider, sizeof(slot.provider), json_cstr(s["provider"], "idfm"));
      copy_field(slot.feed_key, sizeof(slot.feed_key), json_cstr(s["feed_key"]));
      slot.prefer_theoretical = s["prefer_theoretical"] | false;
    }
  }

  apply_display_json(root, cfg);
  return true;
}

bool config_has_lines(const RuntimeConfig& cfg)
{
  for (uint8_t i = 0; i < cfg.slot_count; ++i) {
    if (cfg.slots[i].monitoring_ref[0] != '\0') {
      return true;
    }
  }
  return cfg.idfm_monitoring_ref[0] != '\0';
}

bool config_needs_api_key(const RuntimeConfig& cfg)
{
  if (cfg.slot_count == 0) {
    const TransitProvider p = transit_provider_from_string(cfg.idfm_provider);
    return p == kTransitProviderIdfm;
  }
  for (uint8_t i = 0; i < cfg.slot_count; ++i) {
    const TransitProvider p = transit_provider_from_string(cfg.slots[i].provider);
    if (p == kTransitProviderIdfm) {
      return true;
    }
  }
  return false;
}

bool config_is_valid(const RuntimeConfig& cfg)
{
  if (cfg.wifi_ssid[0] == '\0' || !config_has_lines(cfg)) {
    return false;
  }
  if (config_needs_api_key(cfg) && cfg.idfm_api_key[0] == '\0') {
    return false;
  }
  return true;
}

void log_config_validity(const RuntimeConfig& cfg)
{
  if (cfg.wifi_ssid[0] == '\0') {
    Serial.println(F("[cfg] invalid: wifi_ssid empty"));
  }
  if (config_needs_api_key(cfg) && cfg.idfm_api_key[0] == '\0') {
    Serial.println(F("[cfg] invalid: api key empty (IDFM)"));
  }
  if (!config_has_lines(cfg)) {
    Serial.printf("[cfg] invalid: no lines (slots=%u mon=%.12s)\n",
                  (unsigned)cfg.slot_count,
                  cfg.idfm_monitoring_ref);
  }
}

bool deserialize_config(const char* json, RuntimeConfig& cfg)
{
  const size_t json_len = (json != nullptr) ? strlen(json) : 0;
  if (json_len == 0 || json_len > kMaxStoredJsonBytes) {
    Serial.printf("[cfg] JSON length invalid: %u\n", (unsigned)json_len);
    return false;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, json, json_len);
  if (err) {
    Serial.printf("[cfg] JSON parse error: %s (len=%u heap=%u)\n",
                  err.c_str(),
                  (unsigned)json_len,
                  (unsigned)ESP.getFreeHeap());
    return false;
  }
  cfg = RuntimeConfig{};
  if (!apply_json_object(doc.as<JsonObjectConst>(), cfg)) {
    return false;
  }
  cfg.configured = config_is_valid(cfg);
  if (!cfg.configured) {
    log_config_validity(cfg);
  }
  return cfg.configured;
}

bool serialize_config(const RuntimeConfig& cfg, String& out)
{
  JsonDocument doc;
  doc["wifi_ssid"] = cfg.wifi_ssid;
  doc["wifi_password"] = cfg.wifi_password;
  doc["mqtt_host"] = cfg.mqtt_host;
  doc["mqtt_port"] = cfg.mqtt_port;
  doc["mqtt_user"] = cfg.mqtt_user;
  doc["mqtt_password"] = cfg.mqtt_password;
  doc["mqtt_topic_notify"] = cfg.mqtt_topic_notify;
  doc["idfm_api_key"] = cfg.idfm_api_key;
  doc["transit_api_base"] = cfg.transit_api_base;
  doc["idfm_destination_filter"] = cfg.idfm_destination_filter;
  doc["idfm_monitoring_ref"] = cfg.idfm_monitoring_ref;
  doc["idfm_line_ref"] = cfg.idfm_line_ref;
  doc["idfm_line_code"] = cfg.idfm_line_code;
  doc["idfm_line_label"] = cfg.idfm_line_label;
  doc["idfm_provider"] = cfg.idfm_provider;
  doc["idfm_feed_key"] = cfg.idfm_feed_key;
  doc["idfm_prefer_theoretical"] = cfg.idfm_prefer_theoretical;

  doc["setup_line1"] = cfg.setup_line1;
  doc["setup_line2"] = cfg.setup_line2;
  doc["setup_line3"] = cfg.setup_line3;
  doc["boot_splash_text"] = cfg.boot_splash_text;
  doc["boot_selftest_ms"] = cfg.boot_selftest_ms;
  char color_buf[8];
  snprintf(color_buf, sizeof(color_buf), "#%02X%02X%02X", cfg.color_time_r, cfg.color_time_g, cfg.color_time_b);
  doc["color_time"] = color_buf;
  snprintf(color_buf, sizeof(color_buf), "#%02X%02X%02X", cfg.color_eta_green_r, cfg.color_eta_green_g, cfg.color_eta_green_b);
  doc["color_eta_green"] = color_buf;
  snprintf(color_buf, sizeof(color_buf), "#%02X%02X%02X", cfg.color_eta_orange_r, cfg.color_eta_orange_g, cfg.color_eta_orange_b);
  doc["color_eta_orange"] = color_buf;
  snprintf(color_buf, sizeof(color_buf), "#%02X%02X%02X", cfg.color_eta_red_r, cfg.color_eta_red_g, cfg.color_eta_red_b);
  doc["color_eta_red"] = color_buf;
  snprintf(color_buf, sizeof(color_buf), "#%02X%02X%02X", cfg.color_eta_noinfo_r, cfg.color_eta_noinfo_g, cfg.color_eta_noinfo_b);
  doc["color_eta_noinfo"] = color_buf;
  snprintf(color_buf, sizeof(color_buf), "#%02X%02X%02X", cfg.color_bus_line_r, cfg.color_bus_line_g, cfg.color_bus_line_b);
  doc["color_bus_line"] = color_buf;
  snprintf(color_buf, sizeof(color_buf), "#%02X%02X%02X", cfg.color_blink_on_r, cfg.color_blink_on_g, cfg.color_blink_on_b);
  doc["color_blink_on"] = color_buf;
  snprintf(color_buf, sizeof(color_buf), "#%02X%02X%02X", cfg.color_blink_off_r, cfg.color_blink_off_g, cfg.color_blink_off_b);
  doc["color_blink_off"] = color_buf;
  doc["bus_eta_blink_ms"] = cfg.bus_eta_blink_ms;
  doc["carousel_hold_ms"] = cfg.carousel_hold_ms;
  doc["carousel_hold_no_info_ms"] = cfg.carousel_hold_no_info_ms;
  doc["hide_carousel_no_info"] = cfg.hide_carousel_no_info;
  doc["eta_threshold_green"] = cfg.eta_threshold_green;
  doc["eta_threshold_orange"] = cfg.eta_threshold_orange;
  doc["eta_display_mode"] = eta_display_mode_to_string(cfg.eta_display_mode);
  doc["minutes_quai_max"] = cfg.minutes_quai_max;
  doc["minutes_pch_max"] = cfg.minutes_pch_max;
  doc["eta_label_quai"] = cfg.eta_label_quai;
  doc["eta_label_pch"] = cfg.eta_label_pch;
  doc["eta_quai_show_minutes"] = cfg.eta_quai_show_minutes;
  doc["eta_pch_show_minutes"] = cfg.eta_pch_show_minutes;
  doc["brightness_normal"] = cfg.brightness_normal;
  doc["brightness_dim"] = cfg.brightness_dim;
  doc["dim_hour_start"] = cfg.dim_hour_start;
  doc["dim_hour_end"] = cfg.dim_hour_end;

  JsonArray slots = doc["slots"].to<JsonArray>();
  for (uint8_t i = 0; i < cfg.slot_count; ++i) {
    const RuntimeIdfmSlot& s = cfg.slots[i];
    JsonObject o = slots.add<JsonObject>();
    o["monitoring_ref"] = s.monitoring_ref;
    o["line_ref"] = s.line_ref;
    o["line_code"] = s.line_code;
    o["label"] = s.label;
    o["provider"] = s.provider;
    o["feed_key"] = s.feed_key;
    o["prefer_theoretical"] = s.prefer_theoretical;
  }

  out.clear();
  const size_t n = serializeJson(doc, out);
  if (n == 0) {
    Serial.println(F("[cfg] serializeJson failed (buffer too small?)"));
    return false;
  }
  if (out.length() > kMaxStoredJsonBytes) {
    Serial.printf("[cfg] JSON too large: %u bytes\n", (unsigned)out.length());
    return false;
  }
  return true;
}

bool load_json_from_prefs(Preferences& prefs, String& out)
{
  size_t len = prefs.getBytesLength(kPrefsKeyJson);
  if (len == 0) {
    const String legacy = prefs.getString(kPrefsKeyJson, "");
    if (legacy.length() > 0 && legacy.length() <= kMaxStoredJsonBytes) {
      out = legacy;
      return true;
    }
    return false;
  }
  if (len > kMaxStoredJsonBytes) {
    Serial.printf("[cfg] NVS json_len invalid: %u\n", (unsigned)len);
    return false;
  }
  out.clear();
  out.reserve(len + 1);
  std::unique_ptr<char[]> buf(new char[len + 1]);
  if (!buf) {
    return false;
  }
  const size_t got = prefs.getBytes(kPrefsKeyJson, reinterpret_cast<uint8_t*>(buf.get()), len);
  if (got != len) {
    Serial.printf("[cfg] NVS getBytes mismatch got=%u expected=%u\n", (unsigned)got, (unsigned)len);
    return false;
  }
  buf[len] = '\0';
  out = buf.get();
  return true;
}

bool load_json_from_ffat(String& out)
{
  if (!mount_ffat(false) || !FFat.exists(kFatConfigPath)) {
    return false;
  }
  File f = FFat.open(kFatConfigPath, FILE_READ);
  if (!f) {
    Serial.println(F("[cfg] FFat open read failed"));
    return false;
  }
  const size_t len = f.size();
  if (len == 0 || len > kMaxStoredJsonBytes) {
    f.close();
    Serial.printf("[cfg] FFat json bad len=%u\n", (unsigned)len);
    return false;
  }
  out.clear();
  out.reserve(len + 1);
  std::unique_ptr<char[]> buf(new char[len + 1]);
  if (!buf) {
    f.close();
    return false;
  }
  const size_t got = f.read(reinterpret_cast<uint8_t*>(buf.get()), len);
  f.close();
  if (got != len) {
    Serial.printf("[cfg] FFat read short %u/%u\n", (unsigned)got, (unsigned)len);
    return false;
  }
  buf[len] = '\0';
  out = buf.get();
  return true;
}

bool save_json_to_ffat(const String& json)
{
  if (!mount_ffat(false) && !mount_ffat(true)) {
    return false;
  }
  if (FFat.exists(kFatConfigPath)) {
    FFat.remove(kFatConfigPath);
  }
  File f = FFat.open(kFatConfigPath, FILE_WRITE);
  if (!f) {
    Serial.println(F("[cfg] FFat open write failed"));
    return false;
  }
  const size_t written = f.print(json);
  f.flush();
  f.close();
  if (written != json.length()) {
    Serial.printf("[cfg] FFat write short %u/%u\n", (unsigned)written, (unsigned)json.length());
    return false;
  }

  String verify;
  if (!load_json_from_ffat(verify) || verify != json) {
    Serial.println(F("[cfg] FFat verify failed"));
    return false;
  }
  return true;
}

bool load_stored_json(String& out, const char*& source)
{
  if (load_json_from_ffat(out)) {
    source = "FFat";
    return true;
  }

  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, true) && load_json_from_prefs(prefs, out)) {
    prefs.end();
    source = "NVS";
    return true;
  }
  prefs.end();
  return false;
}

bool save_stored_json(const String& json)
{
  if (save_json_to_ffat(json)) {
    Serial.printf("[cfg] stored on FFat (%u bytes)\n", (unsigned)json.length());
    return true;
  }

  Serial.println(F("[cfg] FFat failed, trying NVS fallback"));
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    Serial.println(F("[cfg] NVS open (write) failed"));
    return false;
  }

  prefs.remove(kPrefsKeyJson);
  size_t written = prefs.putBytes(kPrefsKeyJson,
                                  reinterpret_cast<const uint8_t*>(json.c_str()),
                                  json.length());
  if (written != json.length()) {
    Serial.printf("[cfg] NVS putBytes failed %u/%u\n", (unsigned)written, (unsigned)json.length());
    prefs.end();
    return false;
  }
  prefs.putBool(kPrefsKeyConfigured, true);
  prefs.end();
  Serial.printf("[cfg] stored on NVS (%u bytes)\n", (unsigned)json.length());
  return true;
}

void remove_stored_json()
{
  if (mount_ffat(false) && FFat.exists(kFatConfigPath)) {
    FFat.remove(kFatConfigPath);
  }
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.remove(kPrefsKeyJson);
    prefs.remove(kPrefsKeyConfigured);
    prefs.end();
  }
}

} // namespace

bool runtime_config_apply_json(JsonObjectConst root, RuntimeConfig& cfg)
{
  cfg = RuntimeConfig{};
  if (!apply_json_object(root, cfg)) {
    return false;
  }
  cfg.configured = config_is_valid(cfg);
  return cfg.configured;
}

void runtime_config_init()
{
  g_cfg = RuntimeConfig{};

  String json;
  const char* source = nullptr;
  if (load_stored_json(json, source) && deserialize_config(json.c_str(), g_cfg)) {
    g_cfg.configured = true;
    Serial.printf("[cfg] loaded from %s slots=%u ssid=%.16s\n",
                  source,
                  (unsigned)g_cfg.slot_count,
                  g_cfg.wifi_ssid);
    return;
  }

  if (source != nullptr) {
    Serial.printf("[cfg] %s JSON unusable (len=%u)\n", source, (unsigned)json.length());
  } else {
    Serial.println(F("[cfg] no stored config file"));
  }

  apply_compile_time_defaults(g_cfg);
  g_cfg.configured = false;
  Serial.println(F("[cfg] not configured — setup portal required"));
}

const RuntimeConfig& runtime_config_get()
{
  return g_cfg;
}

bool runtime_config_is_configured()
{
  return g_cfg.configured;
}

bool runtime_config_save(const RuntimeConfig& cfg)
{
  RuntimeConfig to_save = cfg;
  to_save.configured = config_is_valid(to_save);
  if (!to_save.configured) {
    Serial.println(F("[cfg] save rejected: invalid config"));
    log_config_validity(to_save);
    return false;
  }

  String json;
  if (!serialize_config(to_save, json)) {
    return false;
  }

  Serial.printf("[cfg] saving json_len=%u slots=%u heap=%u\n",
                (unsigned)json.length(),
                (unsigned)to_save.slot_count,
                (unsigned)ESP.getFreeHeap());

  if (!save_stored_json(json)) {
    return false;
  }

  g_cfg = to_save;
  g_cfg.configured = true;
  Serial.printf("[cfg] saved OK (slots=%u)\n", (unsigned)g_cfg.slot_count);
  return true;
}

void runtime_config_clear()
{
  remove_stored_json();
  runtime_config_init();
  g_cfg.configured = false;
  Serial.println(F("[cfg] cleared"));
}

const char* cfg_wifi_ssid()
{
  return g_cfg.wifi_ssid;
}
const char* cfg_wifi_password()
{
  return g_cfg.wifi_password;
}
const char* cfg_mqtt_host()
{
  return g_cfg.mqtt_host;
}
uint16_t cfg_mqtt_port()
{
  return g_cfg.mqtt_port;
}
const char* cfg_mqtt_user()
{
  return g_cfg.mqtt_user;
}
const char* cfg_mqtt_password()
{
  return g_cfg.mqtt_password;
}
const char* cfg_mqtt_topic_notify()
{
  return g_cfg.mqtt_topic_notify;
}
const char* cfg_idfm_api_key()
{
  return g_cfg.idfm_api_key;
}
const char* cfg_transit_api_base()
{
  return g_cfg.transit_api_base[0] != '\0' ? g_cfg.transit_api_base : "https://app.mecatran.com";
}
const char* cfg_idfm_destination_filter()
{
  return g_cfg.idfm_destination_filter;
}
const char* cfg_idfm_default_line_label()
{
  return g_cfg.idfm_line_label[0] != '\0' ? g_cfg.idfm_line_label : DEFAULT_BUS_LINE_LABEL;
}

const char* cfg_setup_line1()
{
  return g_cfg.setup_line1;
}
const char* cfg_setup_line2()
{
  return g_cfg.setup_line2;
}
const char* cfg_setup_line3()
{
  return g_cfg.setup_line3;
}
const char* cfg_boot_splash_text()
{
  if (g_cfg.boot_splash_text[0] == '\0') {
    return "TweakFR";
  }
  return g_cfg.boot_splash_text;
}
uint16_t cfg_boot_selftest_ms()
{
  if (g_cfg.boot_selftest_ms == 0) {
    return (uint16_t)HUB75_SELFTEST_STEP_MS;
  }
  return g_cfg.boot_selftest_ms;
}
uint8_t cfg_color_time_r()
{
  return g_cfg.color_time_r;
}
uint8_t cfg_color_time_g()
{
  return g_cfg.color_time_g;
}
uint8_t cfg_color_time_b()
{
  return g_cfg.color_time_b;
}
uint8_t cfg_color_eta_green_r()
{
  return g_cfg.color_eta_green_r;
}
uint8_t cfg_color_eta_green_g()
{
  return g_cfg.color_eta_green_g;
}
uint8_t cfg_color_eta_green_b()
{
  return g_cfg.color_eta_green_b;
}
uint8_t cfg_color_eta_orange_r()
{
  return g_cfg.color_eta_orange_r;
}
uint8_t cfg_color_eta_orange_g()
{
  return g_cfg.color_eta_orange_g;
}
uint8_t cfg_color_eta_orange_b()
{
  return g_cfg.color_eta_orange_b;
}
uint8_t cfg_color_eta_red_r()
{
  return g_cfg.color_eta_red_r;
}
uint8_t cfg_color_eta_red_g()
{
  return g_cfg.color_eta_red_g;
}
uint8_t cfg_color_eta_red_b()
{
  return g_cfg.color_eta_red_b;
}
uint8_t cfg_color_eta_noinfo_r()
{
  return g_cfg.color_eta_noinfo_r;
}
uint8_t cfg_color_eta_noinfo_g()
{
  return g_cfg.color_eta_noinfo_g;
}
uint8_t cfg_color_eta_noinfo_b()
{
  return g_cfg.color_eta_noinfo_b;
}
uint8_t cfg_color_bus_line_r()
{
  return g_cfg.color_bus_line_r;
}
uint8_t cfg_color_bus_line_g()
{
  return g_cfg.color_bus_line_g;
}
uint8_t cfg_color_bus_line_b()
{
  return g_cfg.color_bus_line_b;
}
uint8_t cfg_color_blink_on_r()
{
  return g_cfg.color_blink_on_r;
}
uint8_t cfg_color_blink_on_g()
{
  return g_cfg.color_blink_on_g;
}
uint8_t cfg_color_blink_on_b()
{
  return g_cfg.color_blink_on_b;
}
uint8_t cfg_color_blink_off_r()
{
  return g_cfg.color_blink_off_r;
}
uint8_t cfg_color_blink_off_g()
{
  return g_cfg.color_blink_off_g;
}
uint8_t cfg_color_blink_off_b()
{
  return g_cfg.color_blink_off_b;
}
uint16_t cfg_bus_eta_blink_ms()
{
  return g_cfg.bus_eta_blink_ms > 0 ? g_cfg.bus_eta_blink_ms : (uint16_t)BUS_ETA_BLINK_MS;
}
uint16_t cfg_carousel_hold_ms()
{
  return g_cfg.carousel_hold_ms > 0 ? g_cfg.carousel_hold_ms : (uint16_t)IDFM_HOLD_AFTER_PREFETCH_MS;
}
uint16_t cfg_carousel_hold_no_info_ms()
{
  return g_cfg.carousel_hold_no_info_ms > 0 ? g_cfg.carousel_hold_no_info_ms
                                            : (uint16_t)IDFM_HOLD_NO_INFO_MS;
}
bool cfg_hide_carousel_no_info()
{
  return g_cfg.hide_carousel_no_info;
}
uint8_t cfg_eta_threshold_green()
{
  return g_cfg.eta_threshold_green;
}
uint8_t cfg_eta_threshold_orange()
{
  return g_cfg.eta_threshold_orange;
}
uint8_t cfg_eta_display_mode()
{
  return g_cfg.eta_display_mode;
}
uint8_t cfg_minutes_quai_max()
{
  return g_cfg.minutes_quai_max;
}
uint8_t cfg_minutes_pch_max()
{
  return g_cfg.minutes_pch_max;
}

void cfg_format_bus_eta_text(int minutes,
                             bool prefer_theoretical,
                             const char* departure_iso,
                             char* out,
                             size_t out_size)
{
  (void)prefer_theoretical;
  (void)departure_iso;
  if (out_size == 0) {
    return;
  }
  out[0] = '\0';

  const uint8_t mode = cfg_eta_display_mode();
  const uint8_t quai_max = cfg_minutes_quai_max();
  const uint8_t pch_max = cfg_minutes_pch_max();

  if (mode == kEtaDisplayMinutesOnly) {
    if (minutes <= 0) {
      std::strncpy(out, "--", out_size - 1);
    } else {
      snprintf(out, out_size, "%dm", minutes);
    }
    out[out_size - 1] = '\0';
    return;
  }

  if (minutes <= quai_max) {
    if (cfg_eta_quai_show_minutes() && minutes > 0) {
      snprintf(out, out_size, "%dm", minutes);
    } else {
      const char* label = cfg_eta_label_quai();
      if (label[0] == '\0') {
        label = "QUAI";
      }
      std::strncpy(out, label, out_size - 1);
    }
  } else if (minutes <= pch_max) {
    if (cfg_eta_pch_show_minutes() && minutes > 0) {
      snprintf(out, out_size, "%dm", minutes);
    } else {
      const char* label = cfg_eta_label_pch();
      if (label[0] == '\0') {
        label = "PCH";
      }
      std::strncpy(out, label, out_size - 1);
    }
  } else if (mode == kEtaDisplayLabelsOnly) {
    std::strncpy(out, "--", out_size - 1);
  } else if (minutes > 0) {
    snprintf(out, out_size, "%dm", minutes);
  } else {
    std::strncpy(out, "--", out_size - 1);
  }
  out[out_size - 1] = '\0';
}

const char* cfg_eta_label_quai()
{
  return g_cfg.eta_label_quai;
}
const char* cfg_eta_label_pch()
{
  return g_cfg.eta_label_pch;
}
bool cfg_eta_quai_show_minutes()
{
  return g_cfg.eta_quai_show_minutes;
}
bool cfg_eta_pch_show_minutes()
{
  return g_cfg.eta_pch_show_minutes;
}
uint8_t cfg_brightness_normal()
{
  return g_cfg.brightness_normal;
}
uint8_t cfg_brightness_dim()
{
  return g_cfg.brightness_dim;
}
uint8_t cfg_dim_hour_start()
{
  return g_cfg.dim_hour_start;
}
uint8_t cfg_dim_hour_end()
{
  return g_cfg.dim_hour_end;
}

bool cfg_brightness_is_dim_hour(int hour)
{
  if (hour < 0 || hour > 23) {
    return false;
  }
  const uint8_t start = g_cfg.dim_hour_start;
  const uint8_t end = g_cfg.dim_hour_end;
  if (start == end) {
    return false;
  }
  if (start < end) {
    return (uint8_t)hour >= start && (uint8_t)hour < end;
  }
  return (uint8_t)hour >= start || (uint8_t)hour < end;
}

namespace {

int ascii_tolower(int c)
{
  if (c >= 'A' && c <= 'Z') {
    return c + ('a' - 'A');
  }
  return c;
}

bool str_equals_ascii_ci(const char* a, const char* b)
{
  if (a == nullptr || b == nullptr) {
    return false;
  }
  while (*a != '\0' && *b != '\0') {
    if (ascii_tolower((unsigned char)*a) != ascii_tolower((unsigned char)*b)) {
      return false;
    }
    ++a;
    ++b;
  }
  return *a == '\0' && *b == '\0';
}

bool text_is_eta_label(const char* bus_text, const char* label, const char* fallback)
{
  if (bus_text == nullptr || bus_text[0] == '\0') {
    return false;
  }
  if (label != nullptr && label[0] != '\0' && str_equals_ascii_ci(bus_text, label)) {
    return true;
  }
  return fallback != nullptr && str_equals_ascii_ci(bus_text, fallback);
}

bool text_looks_like_hhmm(const char* bus_text)
{
  if (bus_text == nullptr) {
    return false;
  }
  if (std::strlen(bus_text) != 5 || bus_text[2] != ':') {
    return false;
  }
  return std::isdigit((unsigned char)bus_text[0]) != 0 &&
         std::isdigit((unsigned char)bus_text[1]) != 0 &&
         std::isdigit((unsigned char)bus_text[3]) != 0 &&
         std::isdigit((unsigned char)bus_text[4]) != 0;
}

} // namespace

bool cfg_bus_eta_should_blink(const char* bus_text, int16_t eta_minutes)
{
  if (bus_text == nullptr || bus_text[0] == '\0' || std::strcmp(bus_text, "--") == 0) {
    return false;
  }
  if (cfg_eta_display_mode() == kEtaDisplayMinutesOnly) {
    return false;
  }
  if (text_looks_like_hhmm(bus_text)) {
    return false;
  }

  const uint8_t quai_max = cfg_minutes_quai_max();
  const uint8_t pch_max = cfg_minutes_pch_max();

  if (eta_minutes >= 0) {
    if (eta_minutes <= static_cast<int16_t>(quai_max)) {
      return !cfg_eta_quai_show_minutes();
    }
    if (eta_minutes <= static_cast<int16_t>(pch_max)) {
      return !cfg_eta_pch_show_minutes();
    }
    return false;
  }

  if (!cfg_eta_quai_show_minutes() &&
      text_is_eta_label(bus_text, cfg_eta_label_quai(), "QUAI")) {
    return true;
  }
  if (!cfg_eta_pch_show_minutes() &&
      text_is_eta_label(bus_text, cfg_eta_label_pch(), "PCH")) {
    return true;
  }
  return false;
}
