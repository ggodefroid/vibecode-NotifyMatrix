#pragma once

#include <stddef.h>

/// One PRIM carousel entry. MonitoringRef and LineRef use the STIF/SIRI format.
struct IdfmCarouselSlot {
  const char* monitoring_ref;
  const char* line_ref;
  const char* line_code;
  const char* label;
  const char* provider;
  const char* feed_key;
  bool prefer_theoretical;
};

/// Vue slot pour les appels API (carrousel actif ou config unique).
struct IdfmSlotView {
  const char* monitoring_ref;
  const char* line_ref;
  const char* line_code;
  const char* label;
  const char* provider;
  const char* feed_key;
  bool prefer_theoretical;
};

/// Compile-time carousel from `.env` (build); may be empty when IDFM_SLOT_COUNT=0.
extern const IdfmCarouselSlot kIdfmCarouselSlots[];
extern const size_t kIdfmCarouselCount;

/// Active carousel used at runtime (NVS or compile-time defaults).
void idfm_carousel_apply_runtime();
const IdfmCarouselSlot* idfm_carousel_active_slots();
size_t idfm_carousel_active_count();
