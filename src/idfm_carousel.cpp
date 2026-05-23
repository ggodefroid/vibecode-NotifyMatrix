#include "idfm_carousel.h"

#include "runtime_config.h"

#include <cstring>

#if IDFM_SLOT_COUNT > 0
const IdfmCarouselSlot kIdfmCarouselSlots[] = {
#if IDFM_SLOT_COUNT > 0
    {IDFM_SLOT_0_MONITORING_REF, IDFM_SLOT_0_LINE_REF, IDFM_SLOT_0_LINE_CODE, IDFM_SLOT_0_LABEL, "idfm", nullptr, IDFM_SLOT_0_PREFER_THEORETICAL},
#endif
#if IDFM_SLOT_COUNT > 1
    {IDFM_SLOT_1_MONITORING_REF, IDFM_SLOT_1_LINE_REF, IDFM_SLOT_1_LINE_CODE, IDFM_SLOT_1_LABEL, "idfm", nullptr, IDFM_SLOT_1_PREFER_THEORETICAL},
#endif
#if IDFM_SLOT_COUNT > 2
    {IDFM_SLOT_2_MONITORING_REF, IDFM_SLOT_2_LINE_REF, IDFM_SLOT_2_LINE_CODE, IDFM_SLOT_2_LABEL, "idfm", nullptr, IDFM_SLOT_2_PREFER_THEORETICAL},
#endif
#if IDFM_SLOT_COUNT > 3
    {IDFM_SLOT_3_MONITORING_REF, IDFM_SLOT_3_LINE_REF, IDFM_SLOT_3_LINE_CODE, IDFM_SLOT_3_LABEL, "idfm", nullptr, IDFM_SLOT_3_PREFER_THEORETICAL},
#endif
#if IDFM_SLOT_COUNT > 4
    {IDFM_SLOT_4_MONITORING_REF, IDFM_SLOT_4_LINE_REF, IDFM_SLOT_4_LINE_CODE, IDFM_SLOT_4_LABEL, "idfm", nullptr, IDFM_SLOT_4_PREFER_THEORETICAL},
#endif
#if IDFM_SLOT_COUNT > 5
    {IDFM_SLOT_5_MONITORING_REF, IDFM_SLOT_5_LINE_REF, IDFM_SLOT_5_LINE_CODE, IDFM_SLOT_5_LABEL, "idfm", nullptr, IDFM_SLOT_5_PREFER_THEORETICAL},
#endif
#if IDFM_SLOT_COUNT > 6
    {IDFM_SLOT_6_MONITORING_REF, IDFM_SLOT_6_LINE_REF, IDFM_SLOT_6_LINE_CODE, IDFM_SLOT_6_LABEL, "idfm", nullptr, IDFM_SLOT_6_PREFER_THEORETICAL},
#endif
#if IDFM_SLOT_COUNT > 7
    {IDFM_SLOT_7_MONITORING_REF, IDFM_SLOT_7_LINE_REF, IDFM_SLOT_7_LINE_CODE, IDFM_SLOT_7_LABEL, "idfm", nullptr, IDFM_SLOT_7_PREFER_THEORETICAL},
#endif
#if IDFM_SLOT_COUNT > 8
    {IDFM_SLOT_8_MONITORING_REF, IDFM_SLOT_8_LINE_REF, IDFM_SLOT_8_LINE_CODE, IDFM_SLOT_8_LABEL, "idfm", nullptr, IDFM_SLOT_8_PREFER_THEORETICAL},
#endif
#if IDFM_SLOT_COUNT > 9
    {IDFM_SLOT_9_MONITORING_REF, IDFM_SLOT_9_LINE_REF, IDFM_SLOT_9_LINE_CODE, IDFM_SLOT_9_LABEL, "idfm", nullptr, IDFM_SLOT_9_PREFER_THEORETICAL},
#endif
#if IDFM_SLOT_COUNT > 10
    {IDFM_SLOT_10_MONITORING_REF, IDFM_SLOT_10_LINE_REF, IDFM_SLOT_10_LINE_CODE, IDFM_SLOT_10_LABEL, "idfm", nullptr, IDFM_SLOT_10_PREFER_THEORETICAL},
#endif
#if IDFM_SLOT_COUNT > 11
    {IDFM_SLOT_11_MONITORING_REF, IDFM_SLOT_11_LINE_REF, IDFM_SLOT_11_LINE_CODE, IDFM_SLOT_11_LABEL, "idfm", nullptr, IDFM_SLOT_11_PREFER_THEORETICAL},
#endif
#if IDFM_SLOT_COUNT > 12
    {IDFM_SLOT_12_MONITORING_REF, IDFM_SLOT_12_LINE_REF, IDFM_SLOT_12_LINE_CODE, IDFM_SLOT_12_LABEL, "idfm", nullptr, IDFM_SLOT_12_PREFER_THEORETICAL},
#endif
#if IDFM_SLOT_COUNT > 13
    {IDFM_SLOT_13_MONITORING_REF, IDFM_SLOT_13_LINE_REF, IDFM_SLOT_13_LINE_CODE, IDFM_SLOT_13_LABEL, "idfm", nullptr, IDFM_SLOT_13_PREFER_THEORETICAL},
#endif
#if IDFM_SLOT_COUNT > 14
    {IDFM_SLOT_14_MONITORING_REF, IDFM_SLOT_14_LINE_REF, IDFM_SLOT_14_LINE_CODE, IDFM_SLOT_14_LABEL, "idfm", nullptr, IDFM_SLOT_14_PREFER_THEORETICAL},
#endif
#if IDFM_SLOT_COUNT > 15
    {IDFM_SLOT_15_MONITORING_REF, IDFM_SLOT_15_LINE_REF, IDFM_SLOT_15_LINE_CODE, IDFM_SLOT_15_LABEL, "idfm", nullptr, IDFM_SLOT_15_PREFER_THEORETICAL},
#endif
};

const size_t kIdfmCarouselCount = sizeof(kIdfmCarouselSlots) / sizeof(kIdfmCarouselSlots[0]);
#else
const IdfmCarouselSlot kIdfmCarouselSlots[] = {{nullptr, nullptr, nullptr, nullptr, "idfm", nullptr, false}};
const size_t kIdfmCarouselCount = 0;
#endif

namespace {

static IdfmCarouselSlot s_active_slots[16];
static char s_monitoring_bufs[16][72];
static char s_line_ref_bufs[16][48];
static char s_line_code_bufs[16][16];
static char s_label_bufs[16][16];
static char s_provider_bufs[16][12];
static char s_feed_key_bufs[16][32];
static size_t s_active_count = 0;

} // namespace

void idfm_carousel_apply_runtime()
{
  const RuntimeConfig& cfg = runtime_config_get();
  s_active_count = 0;

  if (cfg.slot_count > 0) {
    const uint8_t n = cfg.slot_count > 16 ? 16 : cfg.slot_count;
    for (uint8_t i = 0; i < n; ++i) {
      const RuntimeIdfmSlot& src = cfg.slots[i];
      std::strncpy(s_monitoring_bufs[i], src.monitoring_ref, sizeof(s_monitoring_bufs[i]) - 1);
      std::strncpy(s_line_ref_bufs[i], src.line_ref, sizeof(s_line_ref_bufs[i]) - 1);
      std::strncpy(s_line_code_bufs[i], src.line_code, sizeof(s_line_code_bufs[i]) - 1);
      std::strncpy(s_label_bufs[i], src.label, sizeof(s_label_bufs[i]) - 1);
      std::strncpy(s_provider_bufs[i], src.provider, sizeof(s_provider_bufs[i]) - 1);
      std::strncpy(s_feed_key_bufs[i], src.feed_key, sizeof(s_feed_key_bufs[i]) - 1);
      s_active_slots[i] = {s_monitoring_bufs[i],
                           s_line_ref_bufs[i],
                           s_line_code_bufs[i],
                           s_label_bufs[i],
                           s_provider_bufs[i],
                           s_feed_key_bufs[i],
                           src.prefer_theoretical};
    }
    s_active_count = n;
    return;
  }

  if (cfg.idfm_monitoring_ref[0] != '\0') {
    std::strncpy(s_monitoring_bufs[0], cfg.idfm_monitoring_ref, sizeof(s_monitoring_bufs[0]) - 1);
    std::strncpy(s_line_ref_bufs[0], cfg.idfm_line_ref, sizeof(s_line_ref_bufs[0]) - 1);
    std::strncpy(s_line_code_bufs[0], cfg.idfm_line_code, sizeof(s_line_code_bufs[0]) - 1);
    std::strncpy(s_label_bufs[0], cfg.idfm_line_label, sizeof(s_label_bufs[0]) - 1);
    std::strncpy(s_provider_bufs[0], cfg.idfm_provider, sizeof(s_provider_bufs[0]) - 1);
    std::strncpy(s_feed_key_bufs[0], cfg.idfm_feed_key, sizeof(s_feed_key_bufs[0]) - 1);
    s_active_slots[0] = {s_monitoring_bufs[0],
                         s_line_ref_bufs[0],
                         s_line_code_bufs[0],
                         s_label_bufs[0],
                         s_provider_bufs[0],
                         s_feed_key_bufs[0],
                         cfg.idfm_prefer_theoretical};
    s_active_count = 1;
  }
}

const IdfmCarouselSlot* idfm_carousel_active_slots()
{
  return s_active_slots;
}

size_t idfm_carousel_active_count()
{
  return s_active_count;
}
