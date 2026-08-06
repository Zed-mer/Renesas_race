#ifndef RF_V18_SOURCE_GATE_H
#define RF_V18_SOURCE_GATE_H

#include <stddef.h>
#include <stdint.h>

#include "rf_v16_roi_postprocess.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RF_V18_SOURCE_PRIMARY UINT8_C(0)
#define RF_V18_SOURCE_V2_VIDEO_FALLBACK UINT8_C(1)
#define RF_V18_QUALITY_NONE UINT8_C(0)
#define RF_V18_QUALITY_NORMAL UINT8_C(1)
#define RF_V18_QUALITY_STRONG UINT8_C(2)

#define RF_V18_DJI_CONTROL_STRONG_CONFIDENCE_Q15 UINT16_C(29490)
#define RF_V18_VIDEO_STRONG_FREQUENCY_EDGE_Q8 INT16_C(384)
#define RF_V18_VIDEO_STRONG_CONTRAST_Q8 INT16_C(512)

typedef struct rf_v18_source_gate_config {
    int32_t weights_q20_per_raw_unit[8];
    int32_t intercept_q20;
    int32_t threshold_q20;
    uint16_t minimum_confidence_q15;
    uint8_t state_evidence_enabled;
    uint8_t reserved;
} rf_v18_source_gate_config_t;

extern const rf_v18_source_gate_config_t g_rf_v18_v2_video_fallback_gate;

int64_t rf_v18_source_gate_score_q20(
    uint16_t confidence_q15,
    const rf_v16_roi_statistics_t *statistics,
    const rf_v18_source_gate_config_t *config
);

int rf_v18_v2_video_fallback_accept(
    uint16_t confidence_q15,
    const rf_v16_roi_statistics_t *statistics
);

/* A fallback may draw a box but always returns QUALITY_NONE for state. */
uint8_t rf_v18_state_quality_tier(
    uint8_t class_id,
    uint8_t source_id,
    uint16_t confidence_q15,
    uint8_t state_roi_decision,
    const rf_v16_roi_statistics_t *statistics
);

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(rf_v18_source_gate_config_t) == 44u,
               "V18 source gate config ABI changed");
#endif

#ifdef __cplusplus
}
#endif

#endif
