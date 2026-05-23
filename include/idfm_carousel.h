#pragma once

#include <stddef.h>

/// One PRIM carousel entry. MonitoringRef and LineRef use the STIF/SIRI format.
struct IdfmCarouselSlot {
  const char* monitoring_ref;
  const char* line_ref;
  const char* line_code;
  const char* label;
  bool prefer_theoretical;
};

extern const IdfmCarouselSlot kIdfmCarouselSlots[];
extern const size_t kIdfmCarouselCount;
