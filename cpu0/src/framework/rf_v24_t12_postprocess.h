#ifndef RF_V24_T12_POSTPROCESS_H
#define RF_V24_T12_POSTPROCESS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RF_V24_T12_HEATMAP_FREQUENCY_BINS 102u
#define RF_V24_T12_HEATMAP_TIME_BINS 58u
#define RF_V24_T12_HEATMAP_BYTES 5916u
#define RF_V24_T12_TILE_SAMPLES 590336u
#define RF_V24_T12_BANDWIDTH_HZ INT32_C(1700000)
#define RF_V24_T12_DURATION_SAMPLES INT32_C(276000)
#define RF_V24_T12_TOP_K_PER_SOURCE 6u
#define RF_V24_T12_MAX_EVENTS 12u
#define RF_V24_T12_EVENT_THRESHOLD_Q15 UINT16_C(17928)

enum rf_v24_t12_source_mask {
    RF_V24_T12_SOURCE_SPECIALIST = 1u << 0,
    RF_V24_T12_SOURCE_V21 = 1u << 1
};

enum rf_v24_t12_event_flags {
    RF_V24_T12_TIME_LEFT_CLIPPED = 1u << 0,
    RF_V24_T12_TIME_RIGHT_CLIPPED = 1u << 1,
    RF_V24_T12_FREQUENCY_LOW_CLIPPED = 1u << 2,
    RF_V24_T12_FREQUENCY_HIGH_CLIPPED = 1u << 3
};

typedef struct rf_v24_t12_event {
    int32_t center_frequency_offset_hz;
    int32_t center_sample;
    int32_t canonical_start_sample;
    int32_t canonical_end_sample;
    int32_t visible_start_sample;
    int32_t visible_end_sample;
    int32_t canonical_frequency_low_offset_hz;
    int32_t canonical_frequency_high_offset_hz;
    int32_t visible_frequency_low_offset_hz;
    int32_t visible_frequency_high_offset_hz;
    uint16_t raw_score_q15;
    uint16_t calibrated_confidence_q15;
    uint8_t source_mask;
    uint8_t event_flags;
    uint8_t heatmap_row;
    uint8_t heatmap_column;
} rf_v24_t12_event_t;

uint16_t rf_v24_t12_specialist_score_q15(int8_t raw_logit);
uint16_t rf_v24_t12_v21_score_q15(int8_t raw_logit);

/*
 * Decode V21 and V24 specialist heatmaps into fixed T12 events. The caller
 * must preserve the V21 T12 output before invoking the specialist model.
 * The specialist is valid only for the two 2.4 GHz capture centers.
 */
size_t rf_v24_t12_postprocess(
    const int8_t *v21_heatmap,
    size_t v21_heatmap_bytes,
    const int8_t *specialist_heatmap,
    size_t specialist_heatmap_bytes,
    uint64_t capture_center_frequency_hz,
    rf_v24_t12_event_t *events,
    size_t event_capacity
);

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(rf_v24_t12_event_t) == 48u,
               "V24 T12 event layout changed");
#endif

#ifdef __cplusplus
}
#endif

#endif
