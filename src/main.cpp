#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <ctime>
#include <esp_rom_sys.h>
#include <esp_system.h>
#include <cstring>
#include <math.h>

#include "app_config.h"
#include "hub75_matrixportal.h"
#include "idfm_carousel.h"
#include "idfm_client.h"
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
      float t = (float)el / (float)NOTIFY_UNSHRINK_MS;
      if (t > 1.f) {
        t = 1.f;
      }
      const float eased_in = t * t * t;
      const float s = (1.f - eased_in) * (float)NOTIFY_SHRINK_PX;
      g_ui.notify_shrink_px = (uint8_t)lrintf(s);
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

static uint32_t g_last_wifi_attempt_ms = 0;
static uint32_t g_last_mqtt_attempt_ms = 0;
static uint32_t g_last_ui_ms = 0;
static uint32_t g_last_log_ms = 0;
static uint8_t g_show_idx = 0;
static bool s_prev_notify_anim = false;

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
    copy_text(g_ui.bus_line, sizeof(g_ui.bus_line), IDFM_LINE_LABEL);
  }
  copy_text(g_ui.bus_text, sizeof(g_ui.bus_text), result.text);
  for (size_t i = 0; i < sizeof(g_ui.bus_text) && g_ui.bus_text[i] != '\0'; i++) {
    if (g_ui.bus_text[i] == '\n' || g_ui.bus_text[i] == '\r') {
      g_ui.bus_text[i] = ' ';
    }
  }
  if (result.ok && result.minutes >= 0) {
    g_ui.bus_eta_minutes = static_cast<int16_t>(result.minutes);
  } else {
    g_ui.bus_eta_minutes = -1;
  }
  g_ui.bus_state = result.ok ? BusState::Ready : BusState::Error;
  g_ui.bus_theoretical = prefer_theoretical;
}

enum class IdfmUiPhase : uint8_t { InitFirst, PrefetchAhead, HoldShowCurrent };

static IdfmUiPhase g_idfm_ui_phase = IdfmUiPhase::InitFirst;
static uint32_t g_idfm_phase_t0_ms = 0;
static uint32_t g_idfm_hold_until_ms = 0;
static IdfmResult g_prefetch{};

struct IdfmSlotView {
  const char* monitoring_ref;
  const char* line_ref;
  const char* line_code;
  const char* label;
  bool prefer_theoretical;
};

static bool time_is_between_7_and_23()
{
  time_t now = time(nullptr);
  struct tm local_time;
  localtime_r(&now, &local_time);
  const int hour = local_time.tm_hour;
  return hour >= 7 && hour < 23;
}

static bool slot_is_hidden_now(const IdfmCarouselSlot& slot)
{
  if (slot.label == nullptr) {
    return false;
  }
  if (std::strcmp(slot.label, "N141") != 0) {
    return false;
  }
  return time_is_between_7_and_23();
}

static size_t carousel_mod()
{
  if (kIdfmCarouselCount == 0) {
    return 1u;
  }
  size_t visible = 0;
  for (size_t i = 0; i < kIdfmCarouselCount; ++i) {
    if (!slot_is_hidden_now(kIdfmCarouselSlots[i])) {
      visible++;
    }
  }
  return visible > 0 ? visible : 1u;
}

static IdfmSlotView slot_at(uint8_t idx)
{
  if (kIdfmCarouselCount > 0) {
    const size_t visible_count = carousel_mod();
    if (visible_count == 0) {
      const IdfmCarouselSlot& s = kIdfmCarouselSlots[0];
      return {s.monitoring_ref,
              s.line_ref,
              s.line_code,
              s.label != nullptr ? s.label : IDFM_LINE_LABEL};
    }
    idx %= (uint8_t)visible_count;
    size_t visible_idx = 0;
    for (size_t i = 0; i < kIdfmCarouselCount; ++i) {
      const IdfmCarouselSlot& s = kIdfmCarouselSlots[i];
      if (slot_is_hidden_now(s)) {
        continue;
      }
      if (visible_idx == idx) {
        return {s.monitoring_ref,
                s.line_ref,
                s.line_code,
                s.label != nullptr ? s.label : IDFM_LINE_LABEL,
                s.prefer_theoretical};
      }
      visible_idx++;
    }
    const IdfmCarouselSlot& s = kIdfmCarouselSlots[0];
    return {s.monitoring_ref,
            s.line_ref,
            s.line_code,
            s.label != nullptr ? s.label : IDFM_LINE_LABEL,
            s.prefer_theoretical};
  }
  return {IDFM_MONITORING_REF,
          IDFM_LINE_REF,
          IDFM_LINE_CODE,
          IDFM_LINE_LABEL,
          IDFM_PREFER_THEORETICAL != 0};
}

static uint8_t prefetch_slot_index()
{
  return (uint8_t)((g_show_idx + 1u) % carousel_mod());
}

static void apply_slot_line_idle_bus_ui(const IdfmSlotView& slot)
{
  if (slot.label != nullptr && slot.label[0] != '\0') {
    copy_text(g_ui.bus_line, sizeof(g_ui.bus_line), slot.label);
  } else {
    copy_text(g_ui.bus_line, sizeof(g_ui.bus_line), IDFM_LINE_LABEL);
  }
  copy_text(g_ui.bus_text, sizeof(g_ui.bus_text), "+1h");
  g_ui.bus_eta_minutes = -1;
  g_ui.bus_state = BusState::Ready;
  g_ui.bus_theoretical = slot.prefer_theoretical;
}

static void set_ui_from_result_for_slot(uint8_t slot_idx, const IdfmResult& r)
{
  const IdfmSlotView s = slot_at(slot_idx);
  set_bus_from_result(r, s.prefer_theoretical);
  if (s.label != nullptr && s.label[0] != '\0') {
    copy_text(g_ui.bus_line, sizeof(g_ui.bus_line), s.label);
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

  if (!g_ui.wifi_connected) {
    return;
  }

  switch (g_idfm_ui_phase) {
  case IdfmUiPhase::InitFirst:
    if (!time_is_valid()) {
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
      if (g_hub.ok()) {
        g_hub.draw_ui(g_ui);
        g_last_ui_ms = now_ms;
      }
      IdfmResult r{};
      const char* line_label =
          (s0.label != nullptr && s0.label[0] != '\0') ? s0.label : IDFM_LINE_LABEL;
      const bool ok = idfm_fetch_next_departure(IDFM_API_KEY,
                                                s0.monitoring_ref,
                                                s0.line_ref,
                                                s0.line_code,
                                                line_label,
                                                s0.prefer_theoretical,
                                                r);
      set_ui_from_result_for_slot(g_show_idx, r);
      (void)ok;
      Serial.printf("[IDFM] init slot=%u ok=%d http=%d text=%.12s\n",
                    (unsigned)g_show_idx,
                    ok ? 1 : 0,
                    r.http_code,
                    r.text);
      s_ui_dirty = true;
      g_idfm_ui_phase = IdfmUiPhase::PrefetchAhead;
    }
    break;

  case IdfmUiPhase::PrefetchAhead:
    if (!time_is_valid()) {
      return;
    }
    if (idfm_defer_http_for_notify()) {
      return;
    }
    {
      (void)update_time_text();
      if (g_hub.ok()) {
        g_hub.draw_ui(g_ui);
        g_last_ui_ms = now_ms;
      }
      const uint8_t nxt = prefetch_slot_index();
      const IdfmSlotView sn = slot_at(nxt);
      const char* line_label =
          (sn.label != nullptr && sn.label[0] != '\0') ? sn.label : IDFM_LINE_LABEL;
      const bool ok = idfm_fetch_next_departure(IDFM_API_KEY,
                                                sn.monitoring_ref,
                                                sn.line_ref,
                                                sn.line_code,
                                                line_label,
                                                sn.prefer_theoretical,
                                                g_prefetch);
      (void)ok;
      Serial.printf("[IDFM] prefetch next=%u (still showing slot=%u) ok=%d http=%d\n",
                    (unsigned)nxt,
                    (unsigned)g_show_idx,
                    ok ? 1 : 0,
                    g_prefetch.http_code);
      g_idfm_hold_until_ms = now_ms + (uint32_t)IDFM_HOLD_AFTER_PREFETCH_MS;
      g_idfm_ui_phase = IdfmUiPhase::HoldShowCurrent;
    }
    break;

  case IdfmUiPhase::HoldShowCurrent:
    if (now_ms < g_idfm_hold_until_ms) {
      return;
    }
    {
      g_show_idx = prefetch_slot_index();
      set_ui_from_result_for_slot(g_show_idx, g_prefetch);
      Serial.printf("[IDFM] show slot=%u after %ums\n",
                    (unsigned)g_show_idx,
                    (unsigned)IDFM_HOLD_AFTER_PREFETCH_MS);
      s_ui_dirty = true;
      g_idfm_ui_phase = IdfmUiPhase::PrefetchAhead;
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
  Serial.printf("WiFi connecting to %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  g_ui.wifi_connected = WiFi.status() == WL_CONNECTED;
  if (g_ui.wifi_connected) {
    Serial.printf("WiFi OK: %s RSSI=%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    Serial.println(F("WiFi KO: timeout"));
  }
}

void ensure_wifi(uint32_t now_ms)
{
  g_ui.wifi_connected = WiFi.status() == WL_CONNECTED;
  if (g_ui.wifi_connected || now_ms - g_last_wifi_attempt_ms < 15000) {
    return;
  }
  g_last_wifi_attempt_ms = now_ms;
  Serial.println(F("WiFi reconnecting..."));
  WiFi.disconnect(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
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

  if (MQTT_HOST[0] == '\0') {
    Serial.println(F("MQTT host is empty; skipping MQTT connect"));
    g_ui.mqtt_connected = false;
    return;
  }
  Serial.printf("MQTT connecting '%s':%d...\n", MQTT_HOST, MQTT_PORT);
  bool connected = false;
  if (has_text(MQTT_USER)) {
    connected = g_mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD);
  } else {
    connected = g_mqtt.connect(MQTT_CLIENT_ID);
  }

  if (connected) {
    g_mqtt.subscribe(MQTT_TOPIC_NOTIFY);
    g_ui.mqtt_connected = true;
    Serial.printf("MQTT OK, subscribe %s\n", MQTT_TOPIC_NOTIFY);
  } else {
    Serial.printf("MQTT KO state=%d\n", g_mqtt.state());
  }
}

void setup_time()
{
  configTzTime(TZ_POSIX, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  struct tm timeinfo {};
  for (int i = 0; i < 80; i++) {
    if (getLocalTime(&timeinfo, 500)) {
      char buf[40];
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &timeinfo);
      Serial.printf("Paris time (TZ): %s\n", buf);
      break;
    }
    delay(100);
  }
}

void setup()
{
  esp_rom_printf("\r\n[NotifyMatrix] setup() START\r\n");

  Serial.begin(115200);
  delay(800);

  const uint32_t wait0 = millis();
  while (!Serial && millis() - wait0 < 5000) {
    delay(20);
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

  copy_text(g_ui.bus_line, sizeof(g_ui.bus_line), IDFM_LINE_LABEL);
  copy_text(g_ui.bus_text, sizeof(g_ui.bus_text), "+1h");
  g_ui.bus_eta_minutes = -1;

  if (!g_hub.begin(128)) {
    Serial.println(F("ERROR: Hub75MatrixPortal::begin() failed"));
  } else {
    g_hub.run_boot_rgb_horizontal_thirds(HUB75_SELFTEST_STEP_MS);
    g_hub.draw_ui(g_ui);
  }

  connect_wifi_blocking();
  setup_time();

  g_idfm_phase_t0_ms = millis();
  apply_slot_line_idle_bus_ui(slot_at(g_show_idx));
  s_ui_dirty = true;

  if (MQTT_HOST[0] != '\0') {
    g_mqtt.setServer(MQTT_HOST, MQTT_PORT);
  } else {
    Serial.println(F("MQTT host empty at startup; MQTT disabled"));
  }
  g_mqtt.setCallback(mqtt_callback);
}

void loop()
{
  const uint32_t now_ms = millis();

  static uint32_t t_led = 0;
  static bool led_on = false;
  if (now_ms - t_led >= 250) {
    t_led = now_ms;
    led_on = !led_on;
    digitalWrite(NM_STATUS_LED, led_on ? HIGH : LOW);
  }

  ensure_wifi(now_ms);
  ensure_mqtt(now_ms);
  if (g_mqtt.connected()) {
    g_mqtt.loop();
  }

  update_notify_visuals(now_ms);
  if (update_time_text()) {
    s_ui_dirty = true;
  }

  poll_idfm_flow(now_ms);

  if (g_hub.ok()) {
    const bool notify_animating =
        g_ui.notification_active &&
        (g_notify_phase != NotifyPhase::Idle || g_ui.notify_scroll_visible);
    const bool anim_ended = s_prev_notify_anim && !notify_animating;
    s_prev_notify_anim = notify_animating;

    const bool blink_active = std::strcmp(g_ui.bus_text, "PCH") == 0 ||
                              std::strcmp(g_ui.bus_text, "QUAI") == 0;
    const bool need_draw = notify_animating || s_ui_dirty || anim_ended || blink_active;
    const uint32_t ui_period =
        (notify_animating || blink_active)
            ? (uint32_t)UI_REFRESH_MS
            : (anim_ended ? 0u : (uint32_t)UI_IDLE_REFRESH_MS);
    if (need_draw &&
        (g_last_ui_ms == 0 || (now_ms - g_last_ui_ms) >= ui_period)) {
      g_last_ui_ms = now_ms;
      g_hub.draw_ui(g_ui);
      if (!notify_animating) {
        s_ui_dirty = false;
      }
    }

    // Adjust brightness based on time: dim from 21:30 to 7:00
    struct tm timeinfo {};
    if (getLocalTime(&timeinfo, 0)) {
      int hour = timeinfo.tm_hour;
      int min = timeinfo.tm_min;
      bool is_dim_time = (hour > 21 || (hour == 21 && min >= 30)) || (hour < 7);
      uint8_t brightness = is_dim_time ? 32 : 255;
      static uint8_t last_brightness = 255;
      if (brightness != last_brightness) {
        g_hub.set_brightness8(brightness);
        last_brightness = brightness;
      }
    }
  }

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
}
