#include "rf_v18_source_gate.h"

#include <limits.h>

const rf_v18_source_gate_config_t g_rf_v18_v2_video_fallback_gate = {
    {INT32_C(0), INT32_C(88), INT32_C(4670), INT32_C(300),
     INT32_C(6974), INT32_C(69), INT32_C(-9689), INT32_C(-122)},
    INT32_C(-5845044),
    INT32_C(303578),
    UINT16_C(6553),
    UINT8_C(0),
    UINT8_C(0),
};

static int64_t rf_v18_add_saturated_i64(int64_t left, int64_t right)
{
    if (right > 0 && left > INT64_MAX - right) {
        return INT64_MAX;
    }
    if (right < 0 && left < INT64_MIN - right) {
        return INT64_MIN;
    }
    return left + right;
}

int64_t rf_v18_source_gate_score_q20(
    uint16_t confidence_q15,
    const rf_v16_roi_statistics_t *statistics,
    const rf_v18_source_gate_config_t *config)
{
    int32_t values[8];
    int64_t score;
    size_t index;
    if (statistics == NULL || config == NULL) {
        return INT64_MIN;
    }
    values[0] = (int32_t)confidence_q15;
    values[1] = statistics->contrast_q8;
    values[2] = statistics->frequency_edge_q8;
    values[3] = statistics->time_edge_q8;
    values[4] = statistics->burstiness_q8;
    values[5] = statistics->occupancy_q15;
    values[6] = statistics->texture_q8;
    values[7] = statistics->visible_fraction_q15;
    score = config->intercept_q20;
    for (index = 0u; index < 8u; ++index) {
        score = rf_v18_add_saturated_i64(
            score,
            (int64_t)values[index] *
                (int64_t)config->weights_q20_per_raw_unit[index]);
    }
    return score;
}

int rf_v18_v2_video_fallback_accept(
    uint16_t confidence_q15,
    const rf_v16_roi_statistics_t *statistics)
{
    return confidence_q15 >=
               g_rf_v18_v2_video_fallback_gate.minimum_confidence_q15 &&
           rf_v18_source_gate_score_q20(
               confidence_q15,
               statistics,
               &g_rf_v18_v2_video_fallback_gate) >=
               g_rf_v18_v2_video_fallback_gate.threshold_q20;
}

uint8_t rf_v18_state_quality_tier(
    uint8_t class_id,
    uint8_t source_id,
    uint16_t confidence_q15,
    uint8_t state_roi_decision,
    const rf_v16_roi_statistics_t *statistics)
{
    if (source_id != RF_V18_SOURCE_PRIMARY ||
        state_roi_decision != RF_V16_ROI_PASS || statistics == NULL ||
        class_id >= RF_V16_CLASS_COUNT) {
        return RF_V18_QUALITY_NONE;
    }
    if (class_id == 0u) {
        return confidence_q15 >= RF_V18_DJI_CONTROL_STRONG_CONFIDENCE_Q15
                   ? RF_V18_QUALITY_STRONG
                   : RF_V18_QUALITY_NORMAL;
    }
    if (class_id == 1u) {
        return statistics->frequency_edge_q8 >=
                       RF_V18_VIDEO_STRONG_FREQUENCY_EDGE_Q8 &&
                       statistics->contrast_q8 >=
                           RF_V18_VIDEO_STRONG_CONTRAST_Q8
                   ? RF_V18_QUALITY_STRONG
                   : RF_V18_QUALITY_NORMAL;
    }
    return RF_V18_QUALITY_STRONG;
}
