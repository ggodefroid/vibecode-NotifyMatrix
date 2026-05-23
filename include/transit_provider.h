#pragma once

#include "idfm_client.h"

#include <cstdint>

/// Réseau de transport pour une ligne du carrousel.
enum TransitProvider : uint8_t {
  kTransitProviderIdfm = 0,
  kTransitProviderNaolib = 1,
  kTransitProviderTul = 2,
};

TransitProvider transit_provider_from_string(const char* s);
const char* transit_provider_to_string(TransitProvider p);

/// Prochain passage (délègue selon le réseau configuré).
bool transit_fetch_next_departure(TransitProvider provider,
                                  const char* api_key,
                                  const char* api_base,
                                  const char* stop_ref,
                                  const char* line_ref,
                                  const char* line_code,
                                  const char* feed_key,
                                  const char* fallback_line_label,
                                  bool prefer_theoretical,
                                  IdfmResult& result);

/// Perturbation / info trafic si disponible pour le réseau.
bool transit_line_has_disruption(TransitProvider provider,
                                 const char* api_key,
                                 const char* api_base,
                                 const char* stop_ref,
                                 const char* line_ref,
                                 const char* line_code,
                                 const char* feed_key,
                                 bool& out_disrupted);
