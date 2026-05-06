#pragma once

#include "app_config.h"

struct IdfmResult {
  bool ok = false;
  int minutes = -1;
  int http_code = 0;
  char line[BUS_LABEL_MAX] = IDFM_LINE_LABEL;
  char text[BUS_STATUS_MAX] = "+1h";
  char error[32] = "";
};

bool idfm_fetch_next_departure(const char* api_key,
                               const char* monitoring_ref,
                               const char* line_ref,
                               const char* line_code_substr,
                               const char* fallback_line_label,
                               bool prefer_theoretical,
                               IdfmResult& result);
