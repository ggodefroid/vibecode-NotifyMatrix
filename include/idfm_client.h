#pragma once

#include "app_config.h"

struct IdfmResult {
  bool ok = false;
  int minutes = -1;
  int http_code = 0;
  char line[BUS_LABEL_MAX] = IDFM_LINE_LABEL;
  char text[BUS_STATUS_MAX] = "--";
  char error[32] = "";
  /// True when PRIM reports an active Perturbation message for this line.
  bool disrupted = false;
};

/// Cached PRIM general-message check (InfoChannel Perturbation). Returns false if the HTTP call failed.
bool idfm_line_has_disruption(const char* api_key, const char* line_ref, bool& out_disrupted);

/// Perturbation depuis le cache uniquement (pas d'appel HTTP). Retourne false si pas en cache.
bool idfm_disruption_from_cache(const char* line_ref, bool& out_disrupted);

void idfm_prim_invalidate_dns();

bool idfm_fetch_next_departure(const char* api_key,
                               const char* monitoring_ref,
                               const char* line_ref,
                               const char* line_code_substr,
                               const char* fallback_line_label,
                               bool prefer_theoretical,
                               IdfmResult& result);

