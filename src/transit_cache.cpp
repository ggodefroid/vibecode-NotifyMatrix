#include "transit_cache.h"

#include <Arduino.h>
#include <cstring>

namespace {

constexpr uint8_t kCacheSlots = 8;
constexpr uint32_t kTtlNaolibMs = 40000u;
constexpr uint32_t kTtlTulMs = 120000u;

struct CacheSlot {
  TransitProvider provider = kTransitProviderIdfm;
  char stop[48] = "";
  char line_ref[48] = "";
  char line_code[16] = "";
  bool prefer_theoretical = false;
  uint32_t expiry_ms = 0;
  IdfmResult result{};
  bool valid = false;
};

CacheSlot g_slots[kCacheSlots];

void copy_field(char* dst, size_t n, const char* src)
{
  if (n == 0) {
    return;
  }
  if (src == nullptr) {
    dst[0] = '\0';
    return;
  }
  std::strncpy(dst, src, n - 1);
  dst[n - 1] = '\0';
}

uint32_t ttl_for(TransitProvider p)
{
  switch (p) {
  case kTransitProviderNaolib:
    return kTtlNaolibMs;
  case kTransitProviderTul:
    return kTtlTulMs;
  default:
    return 0;
  }
}

bool key_match(const CacheSlot& s,
               TransitProvider provider,
               const char* stop_ref,
               const char* line_ref,
               const char* line_code,
               bool prefer_theoretical)
{
  if (!s.valid || s.provider != provider || s.prefer_theoretical != prefer_theoretical) {
    return false;
  }
  if (std::strcmp(s.stop, stop_ref != nullptr ? stop_ref : "") != 0) {
    return false;
  }
  if (std::strcmp(s.line_ref, line_ref != nullptr ? line_ref : "") != 0) {
    return false;
  }
  if (std::strcmp(s.line_code, line_code != nullptr ? line_code : "") != 0) {
    return false;
  }
  return true;
}

} // namespace

bool transit_cache_get(TransitProvider provider,
                       const char* stop_ref,
                       const char* line_ref,
                       const char* line_code,
                       bool prefer_theoretical,
                       IdfmResult& result)
{
  if (provider == kTransitProviderIdfm || ttl_for(provider) == 0) {
    return false;
  }
  const uint32_t now = millis();
  for (uint8_t i = 0; i < kCacheSlots; ++i) {
    CacheSlot& s = g_slots[i];
    if (!s.valid || now >= s.expiry_ms) {
      continue;
    }
    if (key_match(s, provider, stop_ref, line_ref, line_code, prefer_theoretical)) {
      result = s.result;
      return true;
    }
  }
  return false;
}

void transit_cache_put(TransitProvider provider,
                       const char* stop_ref,
                       const char* line_ref,
                       const char* line_code,
                       bool prefer_theoretical,
                       const IdfmResult& result)
{
  if (provider == kTransitProviderIdfm || !result.ok || ttl_for(provider) == 0) {
    return;
  }
  static uint8_t rr = 0;
  CacheSlot& s = g_slots[rr++ % kCacheSlots];
  s.provider = provider;
  copy_field(s.stop, sizeof(s.stop), stop_ref);
  copy_field(s.line_ref, sizeof(s.line_ref), line_ref);
  copy_field(s.line_code, sizeof(s.line_code), line_code);
  s.prefer_theoretical = prefer_theoretical;
  s.result = result;
  s.expiry_ms = millis() + ttl_for(provider);
  s.valid = true;
}

void transit_cache_invalidate_provider(TransitProvider provider)
{
  for (uint8_t i = 0; i < kCacheSlots; ++i) {
    if (g_slots[i].provider == provider) {
      g_slots[i].valid = false;
    }
  }
}
