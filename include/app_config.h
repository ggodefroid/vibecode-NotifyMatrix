#pragma once

// Geometry: two 64x32 HUB75 panels chained horizontally by default.
#ifndef PANEL_MODULE_WIDTH
#define PANEL_MODULE_WIDTH 64
#endif
#ifndef PANEL_MODULE_HEIGHT
#define PANEL_MODULE_HEIGHT 32
#endif
#ifndef PANEL_CHAIN_LENGTH
#define PANEL_CHAIN_LENGTH 2
#endif

#define DISPLAY_TOTAL_WIDTH (PANEL_MODULE_WIDTH * PANEL_CHAIN_LENGTH)
#define DISPLAY_TOTAL_HEIGHT PANEL_MODULE_HEIGHT

// 128x32 layout: left side for the clock/notifications, right side for transit.
#define ZONE_TIME_X 0
#define ZONE_TIME_W 96
#define ZONE_BUS_X ZONE_TIME_W
#define ZONE_BUS_W (DISPLAY_TOTAL_WIDTH - ZONE_TIME_W)

// Vertical HUD offset, as a percentage of DISPLAY_TOTAL_HEIGHT.
// Example: 2 is about 1 px on a 32 px high panel.
#ifndef HUD_VERTICAL_OFFSET_PERCENT
#define HUD_VERTICAL_OFFSET_PERCENT 2
#endif

// MQTT notification animation: the HUD moves up and a scrolling banner appears at the bottom.
#ifndef NOTIFY_SHRINK_PX
#define NOTIFY_SHRINK_PX 7
#endif
#ifndef NOTIFY_SHRINK_MS
#define NOTIFY_SHRINK_MS 520
#endif
#ifndef NOTIFY_UNSHRINK_MS
#define NOTIFY_UNSHRINK_MS 520
#endif
// Scrolling speed from right to left, in pixels per second. Lower is slower.
#ifndef NOTIFY_SCROLL_PPS
#define NOTIFY_SCROLL_PPS 30
#endif

// Values below are overridden by `.env` through scripts/load_env_config.py.
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif

#ifndef MQTT_HOST
#define MQTT_HOST "127.0.0.1"
#endif
#ifndef MQTT_PORT
#define MQTT_PORT 1883
#endif
#ifndef MQTT_USER
#define MQTT_USER ""
#endif
#ifndef MQTT_PASSWORD
#define MQTT_PASSWORD ""
#endif
#ifndef MQTT_TOPIC_NOTIFY
#define MQTT_TOPIC_NOTIFY "notifymatrix/notify"
#endif
#ifndef MQTT_CLIENT_ID
#define MQTT_CLIENT_ID "notifymatrix-s3"
#endif

// Display refresh period during animations. 16 ms is about 60 FPS.
#ifndef UI_REFRESH_MS
#define UI_REFRESH_MS 16
#endif

// Outside animations, redraw no more often than this.
#ifndef UI_IDLE_REFRESH_MS
#define UI_IDLE_REFRESH_MS 250
#endif

// Initial WiFi connection timeout.
#ifndef WIFI_CONNECT_TIMEOUT_MS
#define WIFI_CONNECT_TIMEOUT_MS 15000
#endif

// After fetching the next carousel slot, keep showing the current slot for this long.
// The next line is never displayed before its API response is available.
#ifndef IDFM_HOLD_AFTER_PREFETCH_MS
#define IDFM_HOLD_AFTER_PREFETCH_MS 6000u
#endif

// On first boot, keep the clock and current line visible before the first blocking IDFM call.
#ifndef IDFM_LINE_CHANGE_CLOCK_DWELL_MS
#define IDFM_LINE_CHANGE_CLOCK_DWELL_MS 1500u
#endif

// IDFM / PRIM configuration. In carousel mode these are only used as single-line fallbacks.
#ifndef IDFM_API_KEY
#define IDFM_API_KEY ""
#endif
#ifndef IDFM_MONITORING_REF
#define IDFM_MONITORING_REF ""
#endif
#ifndef IDFM_LINE_REF
#define IDFM_LINE_REF ""
#endif
#ifndef IDFM_LINE_LABEL
#define IDFM_LINE_LABEL DEFAULT_BUS_LINE_LABEL
#endif
#ifndef IDFM_LINE_CODE
#define IDFM_LINE_CODE ""
#endif
#ifndef IDFM_DESTINATION_FILTER
#define IDFM_DESTINATION_FILTER ""
#endif
#ifndef IDFM_PREFER_THEORETICAL
#define IDFM_PREFER_THEORETICAL 0
#endif
#ifndef IDFM_SLOT_COUNT
#define IDFM_SLOT_COUNT 0
#endif

// Maximum displayed/received text sizes.
#ifndef NOTIFICATION_TEXT_MAX
#define NOTIFICATION_TEXT_MAX 64
#endif
#ifndef BUS_LABEL_MAX
#define BUS_LABEL_MAX 12
#endif
#ifndef BUS_STATUS_MAX
#define BUS_STATUS_MAX 8
#endif

// Europe/Paris POSIX timezone: UTC+1 in winter and UTC+2 in summer.
#ifndef TZ_POSIX
#define TZ_POSIX "CET-1CEST-2,M3.5.0/2,M10.5.0/3"
#endif

// Default transit line label for the 32 px right-side area.
#ifndef DEFAULT_BUS_LINE_LABEL
#define DEFAULT_BUS_LINE_LABEL "2225"
#endif

// HUB75 column driver. Keep SHIFTREG by default; try FM6126A or ICN2038S flags if needed.
// Optional latch blanking can reduce artifacts on some panels.
#ifndef PANEL_HUB75_LAT_BLANKING
#define PANEL_HUB75_LAT_BLANKING 0
#endif

// Duration for each boot color-test step. Use 0 to disable the test.
#ifndef HUB75_SELFTEST_STEP_MS
#define HUB75_SELFTEST_STEP_MS 400
#endif

// Some 64x32 1/16-scan panels do not use the E address line.
#ifndef PANEL_HUB75_DISABLE_ROW_E
#define PANEL_HUB75_DISABLE_ROW_E 0
#endif
