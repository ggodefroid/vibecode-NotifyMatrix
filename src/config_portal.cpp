#include "config_portal.h"

#include "app_config.h"
#include "hub75_matrixportal.h"
#include "runtime_config.h"
#include "ui_pump.h"

#include <Arduino.h>

#include "config_portal_html.h"
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>
#include <cstring>

#ifndef CONFIG_PORTAL_AP_SSID
#define CONFIG_PORTAL_AP_SSID "NotifyMatrix-Setup"
#endif
#ifndef CONFIG_PORTAL_AP_PASSWORD
#define CONFIG_PORTAL_AP_PASSWORD "notifymatrix"
#endif

namespace {

WebServer g_server(80);
DNSServer g_dns;
bool g_ap_mode = false;
bool g_web_running = false;
bool g_routes_registered = false;
Hub75MatrixPortal* g_hub = nullptr;

void send_portal_page()
{
  g_server.sendHeader("Cache-Control", "public, max-age=86400");
  g_server.sendHeader("Content-Encoding", "gzip");
  g_server.sendHeader("Vary", "Accept-Encoding");
  g_server.setContentLength(CONFIG_PORTAL_HTML_GZ_LEN);
  g_server.send(200, "text/html; charset=utf-8", "");

  WiFiClient client = g_server.client();
  size_t sent = 0;
  constexpr size_t kChunk = 1024;
  uint8_t buf[kChunk];
  while (sent < CONFIG_PORTAL_HTML_GZ_LEN) {
    const size_t n = (CONFIG_PORTAL_HTML_GZ_LEN - sent) > kChunk ? kChunk : (CONFIG_PORTAL_HTML_GZ_LEN - sent);
    std::memcpy_P(buf, CONFIG_PORTAL_HTML_GZ + sent, n);
    client.write(buf, n);
    sent += n;
    yield();
  }
}

void redirect_to_portal()
{
  const IPAddress ip = g_ap_mode ? WiFi.softAPIP() : WiFi.localIP();
  g_server.sendHeader("Location", String("http://") + ip.toString() + "/", true);
  g_server.send(302, "text/plain", "");
}

void handle_not_found()
{
  if (g_ap_mode && g_server.method() == HTTP_GET) {
    redirect_to_portal();
    return;
  }
  g_server.send(404, "text/plain", "Not found");
}

void handle_api_status()
{
  JsonDocument doc;
  doc["portal"] = g_ap_mode;
  doc["configured"] = runtime_config_is_configured();
  doc["ap_ssid"] = CONFIG_PORTAL_AP_SSID;
  doc["ap_password"] = CONFIG_PORTAL_AP_PASSWORD;
  if (g_ap_mode) {
    doc["device_ip"] = WiFi.softAPIP().toString();
  } else if (WiFi.status() == WL_CONNECTED) {
    doc["device_ip"] = WiFi.localIP().toString();
  } else {
    doc["device_ip"] = "";
  }
  String body;
  serializeJson(doc, body);
  g_server.send(200, "application/json", body);
}

void handle_api_config_get()
{
  const RuntimeConfig& cfg = runtime_config_get();
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
  doc["eta_threshold_green"] = cfg.eta_threshold_green;
  doc["eta_threshold_orange"] = cfg.eta_threshold_orange;
  if (cfg.eta_display_mode == kEtaDisplayMinutesOnly) {
    doc["eta_display_mode"] = "minutes";
  } else if (cfg.eta_display_mode == kEtaDisplayLabelsOnly) {
    doc["eta_display_mode"] = "labels";
  } else {
    doc["eta_display_mode"] = "auto";
  }
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

  String body;
  serializeJson(doc, body);
  g_server.send(200, "application/json", body);
}

void handle_api_config_post()
{
  if (!g_server.hasArg("plain")) {
    g_server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing body\"}");
    return;
  }

  const String body = g_server.arg("plain");
  Serial.printf("[portal] POST /api/config body_len=%u heap=%u\n",
                (unsigned)body.length(),
                (unsigned)ESP.getFreeHeap());

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("[portal] JSON parse error: %s\n", err.c_str());
    g_server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid json\"}");
    return;
  }

  RuntimeConfig cfg{};
  if (!runtime_config_apply_json(doc.as<JsonObjectConst>(), cfg)) {
    Serial.println(F("[portal] config rejected by firmware validation"));
    g_server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid config fields\"}");
    return;
  }

  Serial.printf("[portal] config valid slots=%u ssid=%.16s\n",
                (unsigned)cfg.slot_count,
                cfg.wifi_ssid);

  if (!runtime_config_save(cfg)) {
    g_server.send(500, "application/json", "{\"ok\":false,\"error\":\"nvs save failed\"}");
    return;
  }

  g_server.send(200, "application/json", "{\"ok\":true,\"reboot\":true}");
  g_server.client().flush();
  delay(1500);
  Serial.flush();
  ESP.restart();
}

void handle_api_reset()
{
  runtime_config_clear();
  g_server.send(200, "application/json", "{\"ok\":true,\"reboot\":true}");
  delay(300);
  ESP.restart();
}

void register_web_routes()
{
  if (g_routes_registered) {
    return;
  }
  g_server.on("/", HTTP_GET, send_portal_page);
  g_server.on("/index.html", HTTP_GET, send_portal_page);
  g_server.on("/config", HTTP_GET, send_portal_page);
  g_server.on("/api/status", HTTP_GET, handle_api_status);
  g_server.on("/api/config", HTTP_GET, handle_api_config_get);
  g_server.on("/api/config", HTTP_POST, handle_api_config_post);
  g_server.on("/api/reset", HTTP_POST, handle_api_reset);
  if (g_ap_mode) {
    g_server.on("/generate_204", HTTP_GET, redirect_to_portal);
    g_server.on("/gen_204", HTTP_GET, redirect_to_portal);
    g_server.on("/connecttest.txt", HTTP_GET, redirect_to_portal);
    g_server.on("/hotspot-detect.html", HTTP_GET, redirect_to_portal);
    g_server.on("/library/test/success.html", HTTP_GET, redirect_to_portal);
    g_server.on("/ncsi.txt", HTTP_GET, redirect_to_portal);
  }
  g_server.onNotFound(handle_not_found);
  g_routes_registered = true;
}

void start_http_server()
{
  register_web_routes();
  g_server.begin();
  g_web_running = true;
}

} // namespace

void config_portal_begin(Hub75MatrixPortal* hub)
{
  g_hub = hub;
  g_ap_mode = true;
  g_routes_registered = false;

  WiFi.disconnect(true, true);
  delay(100);
  WiFi.mode(WIFI_AP);
  const bool ap_ok = WiFi.softAP(CONFIG_PORTAL_AP_SSID, CONFIG_PORTAL_AP_PASSWORD);
  delay(200);

  Serial.println(F("[portal] setup mode"));
  Serial.printf("[portal] AP SSID=%s password=%s ok=%d\n",
                CONFIG_PORTAL_AP_SSID,
                CONFIG_PORTAL_AP_PASSWORD,
                ap_ok ? 1 : 0);
  Serial.printf("[portal] open http://%s/\n", WiFi.softAPIP().toString().c_str());

  g_dns.start(53, "*", WiFi.softAPIP());
  start_http_server();
}

void config_web_begin_sta()
{
  if (g_web_running || WiFi.status() != WL_CONNECTED || idfm_http_is_busy()) {
    return;
  }
  g_ap_mode = false;
  g_routes_registered = false;
  start_http_server();
  Serial.printf("[web] config UI http://%s/\n", WiFi.localIP().toString().c_str());
}

void config_web_loop()
{
  if (!g_web_running) {
    return;
  }
  if (g_ap_mode) {
    g_dns.processNextRequest();
  }
  g_server.handleClient();
}

bool config_portal_active()
{
  return g_ap_mode;
}

bool config_web_active()
{
  return g_web_running;
}

void config_portal_draw_matrix()
{
  if (g_hub == nullptr || !g_hub->ok()) {
    return;
  }
  const char* line1 = cfg_setup_line1()[0] != '\0' ? cfg_setup_line1() : CONFIG_PORTAL_AP_SSID;
  const char* line2 = cfg_setup_line2()[0] != '\0' ? cfg_setup_line2() : "mdp: notifymatrix";
  const char* line3 = cfg_setup_line3()[0] != '\0' ? cfg_setup_line3() : "192.168.4.1";
  g_hub->draw_setup_screen(line1, line2, line3);
}
