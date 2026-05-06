#pragma once

#include "app_config.h"

#include <cstdint>

enum class BusState : uint8_t {
  Idle,
  Loading,
  Ready,
  Error,
};

struct UiModel {
  char time_text[8] = "--:--";
  char notification[NOTIFICATION_TEXT_MAX] = "";
  bool notification_active = false;
  /// 0..NOTIFY_SHRINK_PX: height reserved at the bottom for the scrolling banner.
  uint8_t notify_shrink_px = 0;
  /// Left position of the notification text while it scrolls from right to left.
  int16_t notify_scroll_x = 0;
  /// Show the banner text during the scrolling phase only.
  bool notify_scroll_visible = false;

  char bus_line[BUS_LABEL_MAX] = DEFAULT_BUS_LINE_LABEL;
  char bus_text[BUS_STATUS_MAX] = "+1h";
  /// Minutes until departure. >=0 when Ready and valid, -1 otherwise.
  int16_t bus_eta_minutes = -1;
  BusState bus_state = BusState::Idle;
  bool bus_theoretical = false;

  bool wifi_connected = false;
  bool mqtt_connected = false;
  bool time_synced = false;
};
