#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <ctime>
#include <esp_rom_sys.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <lwip/dns.h>
#include <cstring>

#include "app_config.h"
#include "config_portal.h"
#include "hub75_matrixportal.h"
#include "ui_pump.h"
#include "idfm_carousel.h"
#include "idfm_client.h"

void nm_wifi_mark_lost()
{
  idfm_prim_invalidate_dns();
}
#include "runtime_config.h"
#include "transit_provider.h"
#include "ui_model.h"

#if defined(LED_BUILTIN)
#define NM_STATUS_LED LED_BUILTIN
#else
#define NM_STATUS_LED 13
#endif

static Hub75MatrixPortal g_hub;
static WiFiClient g_mqtt_net;
static PubSubClient g_mqtt(g_mqtt_net);
static UiModel g_ui;
static bool s_ui_dirty = true;
static bool s_portal_mode = false;

namespace {

enum class NotifyPhase : uint8_t { Idle, Shrink, Scroll, Unshrink };

NotifyPhase g_notify_phase = NotifyPhase::Idle;
uint32_t g_notify_phase_start_ms = 0;
uint32_t g_notify_scroll_t0_ms = 0;
uint16_t g_notify_text_width_px = 0;

uint16_t notify_measure_text_width_px(const char* text)
{
  if (text == nullptr || text[0] == '\0') {
    return 0;
  }
  return (uint16_t)(std::strlen(text) * 6u + 40u);
}

void show_notification(const char* text)
{
  const unsigned int copy_len = min((unsigned int)std::strlen(text), (unsigned int)(sizeof(g_ui.notification) - 1));
  std::memcpy(g_ui.notification, text, copy_len);
  g_ui.notification[copy_len] = '\0';
  g_ui.notification_active = copy_len > 0;
  g_notify_text_width_px = notify_measure_text_width_px(g_ui.notification);
  g_notify_phase = NotifyPhase::Shrink;
  g_notify_phase_start_ms = millis();
  g_ui.notify_shrink_px = 0;
  g_ui.notify_scroll_x = 0;
  g_ui.notify_scroll_visible = false;
  s_ui_dirty = true;
}

void update_notify_visuals(uint32_t now_ms)
{
  if (!g_ui.notification_active || g_ui.notification[0] == '\0') {
    g_notify_phase = NotifyPhase::Idle;
    g_ui.notify_shrink_px = 0;
    g_ui.notify_scroll_x = 0;
    g_ui.notify_scroll_visible = false;
    return;
  }

  switch (g_notify_phase) {
  case NotifyPhase::Idle:
    break;

  case NotifyPhase::Shrink: {
    const uint32_t el = now_ms - g_notify_phase_start_ms;
    if (el >= (uint32_t)NOTIFY_SHRINK_MS) {
      g_ui.notify_shrink_px = (uint8_t)NOTIFY_SHRINK_PX;
      g_notify_phase = NotifyPhase::Scroll;
      g_notify_scroll_t0_ms = now_ms;
    } else {
      g_ui.notify_shrink_px =
          (uint8_t)((el * (uint32_t)NOTIFY_SHRINK_PX) / (uint32_t)NOTIFY_SHRINK_MS);
    }
    break;
  }

  case NotifyPhase::Scroll: {
    g_ui.notify_shrink_px = (uint8_t)NOTIFY_SHRINK_PX;
    g_ui.notify_scroll_visible = true;
    const uint32_t scroll_el = now_ms - g_notify_scroll_t0_ms;
    const int32_t travel =
        (int32_t)((scroll_el * (uint32_t)NOTIFY_SCROLL_PPS) / 1000u);
    const int32_t start_x = (int32_t)DISPLAY_TOTAL_WIDTH + 8;
    g_ui.notify_scroll_x = (int16_t)(start_x - travel);
    if ((int32_t)g_ui.notify_scroll_x + (int32_t)g_notify_text_width_px < -8) {
      g_notify_phase = NotifyPhase::Unshrink;
      g_notify_phase_start_ms = now_ms;
      g_ui.notify_scroll_visible = false;
    }
    break;
  }

  case NotifyPhase::Unshrink: {
    const uint32_t el = now_ms - g_notify_phase_start_ms;
    if (el >= (uint32_t)NOTIFY_UNSHRINK_MS) {
      g_ui.notify_shrink_px = 0;
      g_ui.notify_scroll_x = 0;
      g_ui.notify_scroll_visible = false;
      g_ui.notification_active = false;
      g_ui.notification[0] = '\0';
      g_notify_phase = NotifyPhase::Idle;
    } else {
      const uint32_t p = (el * 1000u) / (uint32_t)NOTIFY_UNSHRINK_MS;
      const uint32_t eased_in = (p * p * p) / 1000000u;
      const uint32_t remain = 1000u - eased_in;
      g_ui.notify_shrink_px =
          (uint8_t)((remain * (uint32_t)NOTIFY_SHRINK_PX + 500u) / 1000u);
    }
    break;
  }
  }

  if (g_ui.notification_active &&
      (g_notify_phase != NotifyPhase::Idle || g_ui.notify_scroll_visible)) {
    s_ui_dirty = true;
  }
}

/// Do not start a blocking PRIM request while an MQTT notification is animating.
bool idfm_defer_http_for_notify()
{
  return g_ui.notification_active &&
         (g_notify_phase != NotifyPhase::Idle || g_ui.notify_scroll_visible);
}

} // namespace

static uint32_t s_bus_blink_last_phase = 0xFFFFFFFFu;

void ui_reset_bus_eta_blink()
{
  s_bus_blink_last_phase = 0xFFFFFFFFu;
}

void ui_pump_bus_eta_blink()
{
  if (!g_hub.ok()) {
    return;
  }
  if (!cfg_bus_eta_should_blink(g_ui.bus_text, g_ui.bus_eta_minutes)) {
    return;
  }
  const uint32_t blink_ms = (uint32_t)cfg_bus_eta_blink_ms();
  if (blink_ms == 0) {
    return;
  }
  const uint32_t phase = millis() / blink_ms;
  if (phase == s_bus_blink_last_phase) {
    return;
  }
  s_bus_blink_last_phase = phase;
  g_hub.refresh_bus_eta_blink(g_ui);
}

void idfm_yield_ms(unsigned long ms)
{
  const uint32_t t0 = millis();
  while ((unsigned long)(millis() - t0) < ms) {
    ui_pump_bus_eta_blink();
    delay(10);
    yield();
  }
}

static uint8_t g_idfm_http_busy = 0;
static bool s_mqtt_paused_for_http = false;
static uint32_t g_last_mqtt_attempt_ms = 0;
static uint8_t s_brightness_before_http = 0;

static const char* wifi_disconnect_reason_name(int reason)
{
  switch (reason) {
  case 2:
    return "AUTH_EXPIRE";
  case 15:
    return "4WAY_HANDSHAKE_TIMEOUT";
  case 39:
    return "TIMEOUT/RSSI";
  case 202:
    return "AUTH_FAIL";
  case 204:
    return "HANDSHAKE_TIMEOUT";
  case 205:
    return "CONNECTION_FAIL";
  case 200:
    return "BEACON_TIMEOUT";
  case 201:
    return "NO_AP_FOUND";
  default:
    return "OTHER";
  }
}

static bool wifi_has_valid_ip()
{
  return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0u);
}

static void wifi_configure_dns_lwip()
{
  ip_addr_t dns1;
  ip_addr_t dns2;
  IP_ADDR4(&dns1, 1, 1, 1, 1);
  IP_ADDR4(&dns2, 8, 8, 8, 8);
  dns_setserver(0, &dns1);
  dns_setserver(1, &dns2);
}

static void wifi_apply_sta_settings()
{
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.setSleep(WIFI_PS_NONE);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_protocol(WIFI_IF_STA,
                        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
  esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
}


static void wifi_register_events_once();

bool nm_wifi_wait_stable(uint32_t timeout_ms)
{
  const uint32_t deadline = millis() + timeout_ms;
  uint32_t stable_start = 0;
  uint32_t last_reconnect_ms = 0;

  while ((int32_t)(deadline - millis()) > 0) {
    if (wifi_has_valid_ip()) {
      if (stable_start == 0) {
        stable_start = millis();
      } else if (millis() - stable_start >= 1000) {
        wifi_configure_dns_lwip();
        wifi_apply_sta_settings();
        return true;
      }
    } else {
      stable_start = 0;
      if (millis() - last_reconnect_ms >= 2000) {
        last_reconnect_ms = millis();
        Serial.println(F("[WiFi] PRIM: reconnexion..."));
        wifi_register_events_once();
        WiFi.mode(WIFI_STA);
        wifi_apply_sta_settings();
        WiFi.disconnect(false, false);
        WiFi.begin(cfg_wifi_ssid(), cfg_wifi_password());
      }
    }
    idfm_yield_ms(50);
  }

  const bool ok = wifi_has_valid_ip();
  if (!ok) {
    Serial.printf("[WiFi] PRIM: timeout (status=%d ip=%s)\n",
                  (int)WiFi.status(),
                  WiFi.localIP().toString().c_str());
  }
  return ok;
}

void idfm_http_busy_begin()
{
  if (g_idfm_http_busy == 0) {
    WiFi.setSleep(WIFI_PS_NONE);
    wifi_apply_sta_settings();
    s_mqtt_paused_for_http = g_mqtt.connected();
    if (s_mqtt_paused_for_http) {
      g_mqtt.disconnect();
      g_ui.mqtt_connected = false;
    }
    if (g_hub.ok()) {
      s_brightness_before_http = cfg_brightness_normal();
      const uint8_t dim = (s_brightness_before_http > 48) ? 48 : s_brightness_before_http;
      g_hub.set_brightness8(dim);
    }
  }
  if (g_idfm_http_busy < 255) {
    g_idfm_http_busy++;
  }
}

void idfm_http_busy_end()
{
  if (g_idfm_http_busy > 0) {
    g_idfm_http_busy--;
  }
  if (g_idfm_http_busy == 0) {
    if (WiFi.status() == WL_CONNECTED) {
      wifi_apply_sta_settings();
    }
    if (s_mqtt_paused_for_http) {
      s_mqtt_paused_for_http = false;
      g_last_mqtt_attempt_ms = 0;
    }
    if (g_hub.ok() && s_brightness_before_http > 0) {
      g_hub.set_brightness8(s_brightness_before_http);
      s_brightness_before_http = 0;
    }
  }
}

bool idfm_http_is_busy()
{
  return g_idfm_http_busy > 0;
}

static uint32_t g_last_wifi_attempt_ms = 0;
static uint8_t g_wifi_reconnect_failures = 0;
static bool g_wifi_events_registered = false;

static void on_wifi_event(arduino_event_id_t event, arduino_event_info_t info)
{
  switch (event) {
  case ARDUINO_EVENT_WIFI_STA_CONNECTED:
    Serial.println(F("[WiFi] STA connected"));
    wifi_apply_sta_settings();
    break;
  case ARDUINO_EVENT_WIFI_STA_GOT_IP: {
    IPAddress ip(info.got_ip.ip_info.ip.addr);
    if (ip == IPAddress(0u)) {
      ip = WiFi.localIP();
    }
    if (ip != IPAddress(0u)) {
      g_ui.wifi_connected = true;
      g_wifi_reconnect_failures = 0;
      wifi_apply_sta_settings();
      wifi_configure_dns_lwip();
      Serial.printf("[WiFi] OK IP=%s RSSI=%d\n", ip.toString().c_str(), WiFi.RSSI());
    } else {
      g_ui.wifi_connected = false;
      Serial.println(F("[WiFi] GOT_IP but no address yet (DHCP pending)"));
    }
    break;
  }
  case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
    g_ui.wifi_connected = false;
    nm_wifi_mark_lost();
    Serial.printf("[WiFi] disconnected reason=%d (%s)\n",
                  (int)info.wifi_sta_disconnected.reason,
                  wifi_disconnect_reason_name(info.wifi_sta_disconnected.reason));
    break;
  default:
    break;
  }
}

static void wifi_register_events_once()
{
  if (g_wifi_events_registered) {
    return;
  }
  WiFi.onEvent(on_wifi_event);
  g_wifi_events_registered = true;
}

static uint32_t g_last_ui_ms = 0;
static uint32_t g_last_log_ms = 0;
static uint32_t g_last_brightness_check_ms = 0;
static uint8_t g_show_idx = 0;
static bool s_prev_notify_anim = false;
static bool s_prev_wifi_connected = false;

void copy_text(char* dst, size_t dst_size, const char* src)
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

bool has_text(const char* s)
{
  return s != nullptr && s[0] != '\0';
}

bool time_is_valid()
{
  return time(nullptr) > 1700000000;
}

/// Update the clock text and return true only when the displayed text changed.
bool update_time_text()
{
  const bool synced = time_is_valid();
  if (!synced) {
    g_ui.time_synced = false;
    if (std::strcmp(g_ui.time_text, "--:--") != 0) {
      copy_text(g_ui.time_text, sizeof(g_ui.time_text), "--:--");
      return true;
    }
    return false;
  }

  g_ui.time_synced = true;
  struct tm local {};
  const time_t now = time(nullptr);
  localtime_r(&now, &local);
  char next[sizeof(g_ui.time_text)];
  snprintf(next, sizeof(next), "%02d:%02d", local.tm_hour, local.tm_min);
  if (std::strcmp(g_ui.time_text, next) != 0) {
    copy_text(g_ui.time_text, sizeof(g_ui.time_text), next);
    return true;
  }
  return false;
}

void set_bus_from_result(const IdfmResult& result, bool prefer_theoretical)
{
  if (result.ok && result.line[0] != '\0') {
    copy_text(g_ui.bus_line, sizeof(g_ui.bus_line), result.line);
  } else {
    copy_text(g_ui.bus_line, sizeof(g_ui.bus_line), cfg_idfm_default_line_label());
  }

  copy_text(g_ui.bus_text, sizeof(g_ui.bus_text), result.text);

  for (size_t i = 0; i < sizeof(g_ui.bus_text) && g_ui.bus_text[i] != '\0'; i++) {
    if (g_ui.bus_text[i] == '\n' || g_ui.bus_text[i] == '\r') {
      g_ui.bus_text[i] = ' ';
    }
  }
  size_t bus_len = std::strlen(g_ui.bus_text);
  while (bus_len > 0 && g_ui.bus_text[bus_len - 1] == ' ') {
    g_ui.bus_text[--bus_len] = '\0';
  }
  size_t lead = 0;
  while (g_ui.bus_text[lead] == ' ') {
    lead++;
  }
  if (lead > 0) {
    std::memmove(g_ui.bus_text, g_ui.bus_text + lead, bus_len - lead + 1);
  }
  if (result.minutes >= 0) {
    g_ui.bus_eta_minutes = static_cast<int16_t>(result.minutes);
  } else {
    g_ui.bus_eta_minutes = -1;
  }
  const bool no_passage =
      !result.ok &&
      (std::strcmp(result.text, "--") == 0 || std::strcmp(result.text, "ERR") == 0);
  g_ui.bus_state = (result.ok || no_passage) ? BusState::Ready : BusState::Error;
  g_ui.bus_theoretical = prefer_theoretical;
  g_ui.bus_disrupted = result.disrupted;
  ui_reset_bus_eta_blink();
}

enum class IdfmUiPhase : uint8_t { InitFirst, HoldShowCurrent };

static IdfmUiPhase g_idfm_ui_phase = IdfmUiPhase::InitFirst;
static uint32_t g_idfm_phase_t0_ms = 0;
static uint32_t g_idfm_hold_until_ms = 0;
static bool g_idfm_short_hold_after_show = false;
static bool g_idfm_prefetch_ready = false;
static IdfmResult g_prefetch{};
/// Dernière réponse API par index de slot carrousel (0–15).
static bool s_slot_has_info[16] = {true, true, true, true, true, true, true, true,
                                   true, true, true, true, true, true, true, true};

static TransitProvider slot_provider(const IdfmSlotView& slot)
{
  return transit_provider_from_string(slot.provider);
}

static bool slot_label_is_n141(const char* label)
{
  return label != nullptr && label[0] != '\0' && std::strcmp(label, "N141") == 0;
}

static bool fetch_slot_departure(const IdfmSlotView& slot, const char* line_label, IdfmResult& r)
{
  if (transit_fetch_next_departure(slot_provider(slot),
                                   cfg_idfm_api_key(),
                                   cfg_transit_api_base(),
                                   slot.monitoring_ref,
                                   slot.line_ref,
                                   slot.line_code,
                                   slot.feed_key,
                                   line_label,
                                   slot.prefer_theoretical,
                                   r)) {
    return true;
  }

  if (slot.line_code != nullptr && std::strcmp(slot.line_code, "C01639") == 0) {
    static const char* kN141Stops[] = {
        "STIF:StopPoint:Q:471093:",
        "STIF:StopPoint:Q:412406:",
        "STIF:StopArea:SP:68494:",
        nullptr,
    };
    for (size_t i = 0; kN141Stops[i] != nullptr; ++i) {
      if (slot.monitoring_ref != nullptr && std::strcmp(slot.monitoring_ref, kN141Stops[i]) == 0) {
        continue;
      }
      if (transit_fetch_next_departure(slot_provider(slot),
                                       cfg_idfm_api_key(),
                                       cfg_transit_api_base(),
                                       kN141Stops[i],
                                       slot.line_ref,
                                       slot.line_code,
                                       slot.feed_key,
                                       line_label,
                                       slot.prefer_theoretical,
                                       r)) {
        Serial.printf("[IDFM] N141 ok via ref=%s\n", kN141Stops[i]);
        return true;
      }
    }
  }
  return false;
}

static bool idfm_result_has_usable_info(const IdfmResult& r)
{
  if (!r.ok || r.minutes < 0) {
    return false;
  }
  if (r.text[0] == '\0' || std::strcmp(r.text, "--") == 0 || std::strcmp(r.text, "ERR") == 0) {
    return false;
  }
  return true;
}

static void mark_carousel_slot_info(size_t physical_idx, const IdfmResult& r)
{
  if (physical_idx < 16) {
    s_slot_has_info[physical_idx] = idfm_result_has_usable_info(r);
  }
}

static bool slot_is_hidden_now(const IdfmCarouselSlot& slot, size_t physical_idx)
{
  if (cfg_hide_carousel_no_info() && physical_idx < 16 && !s_slot_has_info[physical_idx] &&
      !slot_label_is_n141(slot.label)) {
    return true;
  }
  return false;
}

static size_t carousel_mod()
{
  if (idfm_carousel_active_count() == 0) {
    return 1u;
  }
  size_t visible = 0;
  for (size_t i = 0; i < idfm_carousel_active_count(); ++i) {
    if (!slot_is_hidden_now(idfm_carousel_active_slots()[i], i)) {
      visible++;
    }
  }
  return visible > 0 ? visible : 1u;
}

static IdfmSlotView slot_at(uint8_t idx)
{
  const size_t active_count = idfm_carousel_active_count();
  if (active_count > 0) {
    const size_t visible_count = carousel_mod();
    const IdfmCarouselSlot* slots = idfm_carousel_active_slots();
    if (visible_count == 0) {
      const IdfmCarouselSlot& s = slots[0];
      return {s.monitoring_ref,
              s.line_ref,
              s.line_code,
              s.label != nullptr ? s.label : cfg_idfm_default_line_label(),
              s.provider != nullptr ? s.provider : "idfm",
              s.feed_key,
              s.prefer_theoretical};
    }
    idx %= (uint8_t)visible_count;
    size_t visible_idx = 0;
    for (size_t i = 0; i < active_count; ++i) {
      const IdfmCarouselSlot& s = slots[i];
      if (slot_is_hidden_now(s, i)) {
        continue;
      }
      if (visible_idx == idx) {
        return {s.monitoring_ref,
                s.line_ref,
                s.line_code,
                s.label != nullptr ? s.label : cfg_idfm_default_line_label(),
                s.provider != nullptr ? s.provider : "idfm",
                s.feed_key,
                s.prefer_theoretical};
      }
      visible_idx++;
    }
    const IdfmCarouselSlot& s = slots[0];
    return {s.monitoring_ref,
            s.line_ref,
            s.line_code,
            s.label != nullptr ? s.label : cfg_idfm_default_line_label(),
            s.provider != nullptr ? s.provider : "idfm",
            s.feed_key,
            s.prefer_theoretical};
  }
  const RuntimeConfig& cfg = runtime_config_get();
  return {cfg.idfm_monitoring_ref,
          cfg.idfm_line_ref,
          cfg.idfm_line_code,
          cfg.idfm_line_label,
          cfg.idfm_provider,
          cfg.idfm_feed_key,
          cfg.idfm_prefer_theoretical};
}

static uint8_t prefetch_slot_index()
{
  return (uint8_t)((g_show_idx + 1u) % carousel_mod());
}

static size_t physical_slot_index_for_visible(uint8_t visible_idx)
{
  const size_t active_count = idfm_carousel_active_count();
  if (active_count == 0) {
    return 0;
  }
  visible_idx %= (uint8_t)carousel_mod();
  size_t visible = 0;
  for (size_t i = 0; i < active_count; ++i) {
    if (slot_is_hidden_now(idfm_carousel_active_slots()[i], i)) {
      continue;
    }
    if (visible == visible_idx) {
      return i;
    }
    visible++;
  }
  return 0;
}

static void set_ui_from_result_for_slot(uint8_t slot_idx, const IdfmResult& r)
{
  const IdfmSlotView s = slot_at(slot_idx);
  set_bus_from_result(r, s.prefer_theoretical);
  if (s.label != nullptr && s.label[0] != '\0') {
    copy_text(g_ui.bus_line, sizeof(g_ui.bus_line), s.label);
  }
}

static void apply_slot_line_idle_bus_ui(const IdfmSlotView& slot)
{
  if (slot.label != nullptr && slot.label[0] != '\0') {
    copy_text(g_ui.bus_line, sizeof(g_ui.bus_line), slot.label);
  } else {
    copy_text(g_ui.bus_line, sizeof(g_ui.bus_line), cfg_idfm_default_line_label());
  }
  copy_text(g_ui.bus_text, sizeof(g_ui.bus_text), "--");
  g_ui.bus_eta_minutes = -1;
  g_ui.bus_state = BusState::Ready;
  g_ui.bus_theoretical = slot.prefer_theoretical;
  g_ui.bus_disrupted = false;
}

static void attach_slot_disruption(const IdfmSlotView& slot, IdfmResult& r, bool fetch_http)
{
  const TransitProvider provider = slot_provider(slot);
  if (provider == kTransitProviderNaolib || provider == kTransitProviderTul) {
    return;
  }
  bool disrupted = false;
  if (!fetch_http) {
    if (slot.line_ref != nullptr && slot.line_ref[0] != '\0' &&
        idfm_disruption_from_cache(slot.line_ref, disrupted)) {
      r.disrupted = disrupted;
    }
    return;
  }
  if (transit_line_has_disruption(provider,
                                  cfg_idfm_api_key(),
                                  cfg_transit_api_base(),
                                  slot.monitoring_ref,
                                  slot.line_ref,
                                  slot.line_code,
                                  slot.feed_key,
                                  disrupted)) {
    r.disrupted = disrupted;
  }
}

static void poll_idfm_flow(uint32_t now_ms)
{
  static bool s_prev_time_valid = false;
  const bool tv = time_is_valid();
  if (g_idfm_ui_phase == IdfmUiPhase::InitFirst && tv && !s_prev_time_valid) {
    g_idfm_phase_t0_ms = now_ms;
    s_ui_dirty = true;
  }
  s_prev_time_valid = tv;

  g_ui.wifi_connected = wifi_has_valid_ip();
  if (!g_ui.wifi_connected) {
    return;
  }

  switch (g_idfm_ui_phase) {
  case IdfmUiPhase::InitFirst:
    if (!time_is_valid()) {
      return;
    }
    if (!wifi_has_valid_ip()) {
      return;
    }
    if (now_ms - g_idfm_phase_t0_ms < (uint32_t)IDFM_LINE_CHANGE_CLOCK_DWELL_MS) {
      return;
    }
    if (idfm_defer_http_for_notify()) {
      return;
    }
    {
      const IdfmSlotView s0 = slot_at(g_show_idx);
      apply_slot_line_idle_bus_ui(s0);
      (void)update_time_text();
      s_ui_dirty = true;
      const char* line_label =
          (s0.label != nullptr && s0.label[0] != '\0') ? s0.label : cfg_idfm_default_line_label();
      IdfmResult r{};
      const bool ok = fetch_slot_departure(s0, line_label, r);
      attach_slot_disruption(s0, r, true);
      mark_carousel_slot_info(physical_slot_index_for_visible(g_show_idx), r);
      set_ui_from_result_for_slot(g_show_idx, r);
      Serial.printf("[IDFM] init slot=%u ok=%d http=%d text=%.12s\n",
                    (unsigned)g_show_idx,
                    ok ? 1 : 0,
                    r.http_code,
                    r.text);
      g_idfm_short_hold_after_show = !idfm_result_has_usable_info(r);
      const uint32_t hold_ms = g_idfm_short_hold_after_show
                                   ? (uint32_t)cfg_carousel_hold_no_info_ms()
                                   : (uint32_t)cfg_carousel_hold_ms();
      g_idfm_prefetch_ready = false;
      g_idfm_hold_until_ms = now_ms + hold_ms;
      g_idfm_ui_phase = IdfmUiPhase::HoldShowCurrent;
      s_ui_dirty = true;
    }
    break;

  case IdfmUiPhase::HoldShowCurrent:
    if (!g_idfm_prefetch_ready) {
      if (!time_is_valid()) {
        return;
      }
      if (idfm_defer_http_for_notify()) {
        return;
      }
      const uint8_t nxt = prefetch_slot_index();
      const IdfmSlotView sn = slot_at(nxt);
      const char* line_label =
          (sn.label != nullptr && sn.label[0] != '\0') ? sn.label : cfg_idfm_default_line_label();
      const bool ok = fetch_slot_departure(sn, line_label, g_prefetch);
      attach_slot_disruption(sn, g_prefetch, false);
      mark_carousel_slot_info(physical_slot_index_for_visible(nxt), g_prefetch);
      Serial.printf("[IDFM] prefetch next=%u (showing slot=%u) ok=%d http=%d\n",
                    (unsigned)nxt,
                    (unsigned)g_show_idx,
                    ok ? 1 : 0,
                    g_prefetch.http_code);
      g_idfm_prefetch_ready = true;
      return;
    }
    if (now_ms < g_idfm_hold_until_ms) {
      return;
    }
    {
      g_show_idx = prefetch_slot_index();
      attach_slot_disruption(slot_at(g_show_idx), g_prefetch, true);
      set_ui_from_result_for_slot(g_show_idx, g_prefetch);
      g_idfm_short_hold_after_show = !idfm_result_has_usable_info(g_prefetch);
      const uint32_t hold_ms = g_idfm_short_hold_after_show
                                   ? (uint32_t)cfg_carousel_hold_no_info_ms()
                                   : (uint32_t)cfg_carousel_hold_ms();
      g_idfm_prefetch_ready = false;
      g_idfm_hold_until_ms = now_ms + hold_ms;
      Serial.printf("[IDFM] show slot=%u no_info=%d\n",
                    (unsigned)g_show_idx,
                    g_idfm_short_hold_after_show ? 1 : 0);
      s_ui_dirty = true;
    }
    break;
  }
}

void mqtt_callback(char* topic, byte* payload, unsigned int length)
{
  (void)topic;
  const unsigned int copy_len = min(length, (unsigned int)(sizeof(g_ui.notification) - 1));
  memcpy(g_ui.notification, payload, copy_len);
  g_ui.notification[copy_len] = '\0';
  g_ui.notification_active = copy_len > 0;
  g_notify_text_width_px = notify_measure_text_width_px(g_ui.notification);
  g_notify_phase = NotifyPhase::Shrink;
  g_notify_phase_start_ms = millis();
  g_ui.notify_shrink_px = 0;
  g_ui.notify_scroll_x = 0;
  g_ui.notify_scroll_visible = false;
  s_ui_dirty = true;
  Serial.printf("MQTT notification: %s\n", g_ui.notification);
}

void connect_wifi_blocking()
{
  wifi_register_events_once();
  Serial.printf("WiFi connecting to %s\n", cfg_wifi_ssid());
  WiFi.mode(WIFI_STA);
  wifi_apply_sta_settings();
  WiFi.begin(cfg_wifi_ssid(), cfg_wifi_password());

  const uint32_t start = millis();
  while (millis() - start < (uint32_t)WIFI_IP_WAIT_TIMEOUT_MS) {
    if (wifi_has_valid_ip()) {
      break;
    }
    delay(100);
  }

  g_ui.wifi_connected = wifi_has_valid_ip();
  if (g_ui.wifi_connected) {
    g_wifi_reconnect_failures = 0;
    wifi_apply_sta_settings();
    wifi_configure_dns_lwip();
    Serial.printf("WiFi OK: %s RSSI=%d\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.RSSI());
  } else {
    Serial.printf("WiFi KO: timeout status=%d ip=%s\n",
                  (int)WiFi.status(),
                  WiFi.localIP().toString().c_str());
  }
}

void ensure_wifi(uint32_t now_ms)
{
  const wl_status_t st = WiFi.status();
  g_ui.wifi_connected = wifi_has_valid_ip();
  if (g_ui.wifi_connected) {
    static uint32_t s_last_ps_refresh_ms = 0;
    if (now_ms - s_last_ps_refresh_ms >= 30000) {
      s_last_ps_refresh_ms = now_ms;
      wifi_apply_sta_settings();
    }
    return;
  }

  const uint32_t retry_ms =
      (g_wifi_reconnect_failures < 3) ? 5000u : 15000u;
  if (now_ms - g_last_wifi_attempt_ms < retry_ms) {
    return;
  }
  g_last_wifi_attempt_ms = now_ms;
  g_wifi_reconnect_failures++;

  Serial.printf("[WiFi] reconnect status=%d try=%u ssid=%.16s\n",
                (int)st,
                (unsigned)g_wifi_reconnect_failures,
                cfg_wifi_ssid());

  wifi_register_events_once();
  WiFi.mode(WIFI_STA);
  wifi_apply_sta_settings();

  if (st == WL_DISCONNECTED || st == WL_CONNECTION_LOST || st == WL_IDLE_STATUS) {
    if (!WiFi.reconnect()) {
      WiFi.disconnect(false, false);
      WiFi.begin(cfg_wifi_ssid(), cfg_wifi_password());
    }
  } else {
    WiFi.disconnect(false, false);
    WiFi.begin(cfg_wifi_ssid(), cfg_wifi_password());
  }
}

void ensure_mqtt(uint32_t now_ms)
{
  if (!g_ui.wifi_connected) {
    g_ui.mqtt_connected = false;
    return;
  }
  if (g_mqtt.connected()) {
    g_ui.mqtt_connected = true;
    return;
  }
  g_ui.mqtt_connected = false;
  if (now_ms - g_last_mqtt_attempt_ms < 5000) {
    return;
  }
  g_last_mqtt_attempt_ms = now_ms;

  if (cfg_mqtt_host()[0] == '\0') {
    Serial.println(F("MQTT host is empty; skipping MQTT connect"));
    g_ui.mqtt_connected = false;
    return;
  }
  Serial.printf("MQTT connecting '%s':%d...\n", cfg_mqtt_host(), (int)cfg_mqtt_port());
  bool connected = false;
  if (has_text(cfg_mqtt_user())) {
    connected = g_mqtt.connect(MQTT_CLIENT_ID, cfg_mqtt_user(), cfg_mqtt_password());
  } else {
    connected = g_mqtt.connect(MQTT_CLIENT_ID);
  }

  if (connected) {
    g_mqtt.subscribe(cfg_mqtt_topic_notify());
    g_ui.mqtt_connected = true;
    Serial.printf("MQTT OK, subscribe %s\n", cfg_mqtt_topic_notify());
  } else {
    Serial.printf("MQTT KO state=%d\n", g_mqtt.state());
  }
}

void setup_time()
{
  configTzTime(TZ_POSIX, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  struct tm timeinfo {};
  for (int i = 0; i < 20; i++) {
    if (getLocalTime(&timeinfo, 100)) {
      Serial.println(F("Time synced"));
      break;
    }
    delay(50);
  }
}

void setup()
{
  esp_rom_printf("\r\n[NotifyMatrix] setup() START\r\n");

  Serial.begin(115200);
  delay(500);

  const uint32_t wait0 = millis();
  while (!Serial && millis() - wait0 < 2000) {
    delay(10);
  }

  pinMode(NM_STATUS_LED, OUTPUT);
  digitalWrite(NM_STATUS_LED, HIGH);

  Serial.println();
  Serial.printf("esp_reset_reason()=%d\n", (int)esp_reset_reason());
  Serial.println(F("NotifyMatrix - app"));
  Serial.printf("Geometry: module=%dx%d chain=%d total=%dx%d\n",
                PANEL_MODULE_WIDTH,
                PANEL_MODULE_HEIGHT,
                PANEL_CHAIN_LENGTH,
                DISPLAY_TOTAL_WIDTH,
                DISPLAY_TOTAL_HEIGHT);

  runtime_config_init();

  copy_text(g_ui.bus_line, sizeof(g_ui.bus_line), cfg_idfm_default_line_label());
  copy_text(g_ui.bus_text, sizeof(g_ui.bus_text), "--");
  g_ui.bus_eta_minutes = -1;

  if (!g_hub.begin(cfg_brightness_normal())) {
    Serial.println(F("ERROR: Hub75MatrixPortal::begin() failed"));
  } else {
    const uint32_t boot_step_ms = cfg_boot_selftest_ms();
    if (boot_step_ms > 0) {
      g_hub.run_boot_rgb_horizontal_thirds(boot_step_ms);
    }
    g_hub.draw_ui(g_ui);
  }

  if (!runtime_config_is_configured()) {
    Serial.println(F("First boot: configuration portal"));
    config_portal_begin(&g_hub);
    s_portal_mode = true;
    config_portal_draw_matrix();
    return;
  }

  idfm_carousel_apply_runtime();
  connect_wifi_blocking();
  if (g_ui.wifi_connected) {
    (void)nm_wifi_wait_stable(5000);
  }
  setup_time();

  g_idfm_phase_t0_ms = millis();
  apply_slot_line_idle_bus_ui(slot_at(g_show_idx));
  s_ui_dirty = true;

  if (cfg_mqtt_host()[0] != '\0') {
    g_mqtt.setServer(cfg_mqtt_host(), cfg_mqtt_port());
  } else {
    Serial.println(F("MQTT host empty at startup; MQTT disabled"));
  }
  g_mqtt.setCallback(mqtt_callback);

  if (g_ui.wifi_connected && wifi_has_valid_ip()) {
    config_web_begin_sta();
  }
}

void loop()
{
  const uint32_t now_ms = millis();

  if (s_portal_mode) {
    static uint32_t t_matrix = 0;
    config_web_loop();
    if (now_ms - t_matrix >= 800) {
      t_matrix = now_ms;
      config_portal_draw_matrix();
    }
    delay(1);
    return;
  }

  static uint32_t t_led = 0;
  static bool led_on = false;
  if (now_ms - t_led >= 250) {
    t_led = now_ms;
    led_on = !led_on;
    digitalWrite(NM_STATUS_LED, led_on ? HIGH : LOW);
  }

  ensure_wifi(now_ms);
  if (g_ui.wifi_connected && !s_prev_wifi_connected) {
    show_notification("WiFi OK");
    config_web_begin_sta();
  }
  s_prev_wifi_connected = g_ui.wifi_connected;
  config_web_loop();
  if (!idfm_http_is_busy()) {
    ensure_mqtt(now_ms);
    if (g_mqtt.connected()) {
      g_mqtt.loop();
    }
  }

  update_notify_visuals(now_ms);
  if (update_time_text()) {
    s_ui_dirty = true;
  }

  ui_pump_bus_eta_blink();

  if (g_hub.ok()) {
    const bool notify_animating =
        g_ui.notification_active &&
        (g_notify_phase != NotifyPhase::Idle || g_ui.notify_scroll_visible);
    const bool anim_ended = s_prev_notify_anim && !notify_animating;
    s_prev_notify_anim = notify_animating;

    const bool disruption_blink = g_ui.bus_disrupted;
    const bool eta_blink_active =
        cfg_bus_eta_should_blink(g_ui.bus_text, g_ui.bus_eta_minutes);
    const bool need_full_draw =
        notify_animating || s_ui_dirty || anim_ended || disruption_blink;
    uint32_t ui_period = (uint32_t)UI_IDLE_REFRESH_MS;
    if (notify_animating || disruption_blink) {
      ui_period = (uint32_t)UI_REFRESH_MS;
    } else if (anim_ended) {
      ui_period = 0u;
    }
    if (need_full_draw &&
        (g_last_ui_ms == 0 || (now_ms - g_last_ui_ms) >= ui_period)) {
      g_last_ui_ms = now_ms;
      g_hub.draw_ui(g_ui);
      if (!notify_animating) {
        s_ui_dirty = false;
      }
      if (eta_blink_active) {
        ui_reset_bus_eta_blink();
      }
    } else if (eta_blink_active) {
      ui_pump_bus_eta_blink();
    }

    // Adjust brightness based on time: brighter in daytime, lower in evening/night.
    // Check less frequently to reduce overhead in the main loop.
    if (now_ms - g_last_brightness_check_ms >= 5000) {
      g_last_brightness_check_ms = now_ms;
      struct tm timeinfo {};
      if (getLocalTime(&timeinfo, 0)) {
        const int hour = timeinfo.tm_hour;
        const bool is_dim_time = cfg_brightness_is_dim_hour(hour);
        const uint8_t brightness =
            is_dim_time ? cfg_brightness_dim() : cfg_brightness_normal();
        static uint8_t last_brightness = 0;
        if (last_brightness == 0) {
          last_brightness = cfg_brightness_normal();
        }
        if (brightness != last_brightness) {
          g_hub.set_brightness8(brightness);
          last_brightness = brightness;
        }
      }
    }
  }

  poll_idfm_flow(now_ms);
  ui_pump_bus_eta_blink();

  if (now_ms - g_last_log_ms >= 10000) {
    g_last_log_ms = now_ms;
    Serial.printf("[loop] ms=%lu heap=%u wifi=%d mqtt=%d time=%d bus=%s/%s\n",
                  (unsigned long)now_ms,
                  (unsigned)esp_get_free_heap_size(),
                  g_ui.wifi_connected ? 1 : 0,
                  g_ui.mqtt_connected ? 1 : 0,
                  g_ui.time_synced ? 1 : 0,
                  g_ui.bus_line,
                  g_ui.bus_text);
  }

  yield();
}

