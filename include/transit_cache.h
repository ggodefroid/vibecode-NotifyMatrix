#pragma once

#include "idfm_client.h"
#include "transit_provider.h"

/// Cache court des prochains départs (évite les doubles requêtes HTTP en carrousel).
bool transit_cache_get(TransitProvider provider,
                       const char* stop_ref,
                       const char* line_ref,
                       const char* line_code,
                       bool prefer_theoretical,
                       IdfmResult& result);

void transit_cache_put(TransitProvider provider,
                       const char* stop_ref,
                       const char* line_ref,
                       const char* line_code,
                       bool prefer_theoretical,
                       const IdfmResult& result);

void transit_cache_invalidate_provider(TransitProvider provider);
