#include "transit_provider.h"

#include "naolib_client.h"
#include "transit_cache.h"
#include "tul_client.h"

#include <cstring>

TransitProvider transit_provider_from_string(const char* s)
{
  if (s == nullptr || s[0] == '\0') {
    return kTransitProviderIdfm;
  }
  if (std::strcmp(s, "naolib") == 0 || std::strcmp(s, "nantes") == 0 || std::strcmp(s, "tan") == 0) {
    return kTransitProviderNaolib;
  }
  if (std::strcmp(s, "tul") == 0 || std::strcmp(s, "laval") == 0) {
    return kTransitProviderTul;
  }
  return kTransitProviderIdfm;
}

const char* transit_provider_to_string(TransitProvider p)
{
  switch (p) {
  case kTransitProviderNaolib:
    return "naolib";
  case kTransitProviderTul:
    return "tul";
  default:
    return "idfm";
  }
}

bool transit_fetch_next_departure(TransitProvider provider,
                                  const char* api_key,
                                  const char* api_base,
                                  const char* stop_ref,
                                  const char* line_ref,
                                  const char* line_code,
                                  const char* feed_key,
                                  const char* fallback_line_label,
                                  bool prefer_theoretical,
                                  IdfmResult& result)
{
  if (transit_cache_get(provider, stop_ref, line_ref, line_code, prefer_theoretical, result)) {
    return result.ok;
  }

  bool ok = false;
  switch (provider) {
  case kTransitProviderNaolib:
    (void)prefer_theoretical;
    (void)line_ref;
    (void)api_key;
    (void)api_base;
    (void)feed_key;
    ok = naolib_fetch_next_departure(stop_ref, line_code, fallback_line_label, result);
    break;
  case kTransitProviderTul:
    (void)line_ref;
    ok = tul_fetch_next_departure(api_key,
                                  api_base,
                                  stop_ref,
                                  line_code,
                                  feed_key,
                                  fallback_line_label,
                                  prefer_theoretical,
                                  result);
    break;
  default:
    (void)api_base;
    (void)feed_key;
    ok = idfm_fetch_next_departure(api_key,
                                   stop_ref,
                                   line_ref,
                                   line_code,
                                   fallback_line_label,
                                   prefer_theoretical,
                                   result);
    break;
  }

  if (ok) {
    transit_cache_put(provider, stop_ref, line_ref, line_code, prefer_theoretical, result);
  }
  return ok;
}

bool transit_line_has_disruption(TransitProvider provider,
                                 const char* api_key,
                                 const char* api_base,
                                 const char* stop_ref,
                                 const char* line_ref,
                                 const char* line_code,
                                 const char* feed_key,
                                 bool& out_disrupted)
{
  switch (provider) {
  case kTransitProviderNaolib:
    (void)api_key;
    (void)api_base;
    (void)line_ref;
    (void)feed_key;
    return naolib_line_has_disruption(stop_ref, line_code, out_disrupted);
  case kTransitProviderTul:
    (void)line_ref;
    (void)line_code;
    return tul_line_has_disruption(api_key, api_base, stop_ref, feed_key, out_disrupted);
  default:
    (void)api_base;
    (void)feed_key;
    (void)stop_ref;
    (void)line_code;
    return idfm_line_has_disruption(api_key, line_ref, out_disrupted);
  }
}
