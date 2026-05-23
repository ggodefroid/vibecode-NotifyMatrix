#pragma once

#include "idfm_client.h"

bool naolib_fetch_next_departure(const char* stop_code,
                                 const char* line_code,
                                 const char* fallback_line_label,
                                 IdfmResult& result);

bool naolib_line_has_disruption(const char* stop_code, const char* line_code, bool& out_disrupted);
