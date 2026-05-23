#pragma once

#include "idfm_client.h"

#ifndef TUL_GTFS_ZIP_URL
#define TUL_GTFS_ZIP_URL \
  "https://s3.eu-west-1.amazonaws.com/files.orchestra.ratpdev.com/networks/rd-laval/exports/pan.zip"
#endif

/// Horaires théoriques GTFS (data.gouv / TUL). api_key, api_base et feed_key sont ignorés.
bool tul_fetch_next_departure(const char* api_key,
                              const char* api_base,
                              const char* stop_id,
                              const char* line_code,
                              const char* feed_key,
                              const char* fallback_line_label,
                              bool prefer_theoretical,
                              IdfmResult& result);

/// Pas de perturbations temps réel avec le flux GTFS seul.
bool tul_line_has_disruption(const char* api_key,
                             const char* api_base,
                             const char* stop_id,
                             const char* feed_key,
                             bool& out_disrupted);
