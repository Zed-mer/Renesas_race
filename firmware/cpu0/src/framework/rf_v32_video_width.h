#ifndef RF_V32_VIDEO_WIDTH_H
#define RF_V32_VIDEO_WIDTH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RF_V32_SOURCE_FREQUENCY_BINS 204u
#define RF_V32_SOURCE_TIME_BINS 115u
#define RF_V32_SOURCE_CHANNELS 4u
#define RF_V32_SOURCE_BYTES 93840u
#define RF_V32_ROI_FREQUENCY_BINS 96u
#define RF_V32_ROI_TIME_BINS 24u
#define RF_V32_ROI_CHANNELS 5u
#define RF_V32_ROI_BYTES 11520u
#define RF_V32_RELIABLE_FIRST_ROW 7
#define RF_V32_RELIABLE_END_ROW 197
#define RF_V32_MASK_VALID_CODE INT8_C(7)
#define RF_V32_MASK_INVALID_CODE INT8_C(-7)
#define RF_V32_WIDTH_10MHZ_HZ INT32_C(10000000)
#define RF_V32_WIDTH_20MHZ_HZ INT32_C(20000000)
#define RF_V32_CNN_DECISION_CODE INT8_C(-18)
#define RF_V32_CNN_CONFIDENT_10_MAX_CODE INT8_C(-94)
#define RF_V32_CNN_CONFIDENT_20_MIN_CODE INT8_C(8)
#define RF_V32_CNN_PRIMARY UINT8_C(1)
#define RF_V32_CPU_SHOULDER_DIAGNOSTIC_OR_FALLBACK UINT8_C(1)

typedef struct rf_v32_width_track {
    int32_t bandwidth_hz;
    int16_t evidence_q8;
    uint8_t observation_count;
    uint8_t reserved;
} rf_v32_width_track_t;

typedef struct rf_v32_cpu_width_evidence {
    uint8_t available;
    uint8_t shoulder_rows_above;
    uint8_t shoulder_rows_total;
    uint8_t reserved;
    int32_t bandwidth_hz;
} rf_v32_cpu_width_evidence_t;

/* center_row_q8/center_column_q8 use input-grid coordinates in Q8.8. */
int rf_v32_extract_width_roi(
    const int8_t *source_nhwc,
    size_t source_bytes,
    int32_t center_row_q8,
    int32_t center_column_q8,
    int8_t *roi_nhwc,
    size_t roi_bytes);

/* Returns 1 only when both 10/20 MHz shoulder regions are fully captured. */
int rf_v32_cpu_width_classify(
    const int8_t *roi_nhwc,
    size_t roi_bytes,
    rf_v32_cpu_width_evidence_t *evidence);

void rf_v32_width_track_init(rf_v32_width_track_t *track);

/* CNN is authoritative when both are valid; CPU is a fault fallback only. */
int32_t rf_v32_width_track_apply(
    rf_v32_width_track_t *track,
    int cpu_valid,
    int32_t cpu_bandwidth_hz,
    int cnn_valid,
    int8_t cnn_output_code);

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(rf_v32_width_track_t) == 8u,
               "V32 width-track ABI changed");
_Static_assert(sizeof(rf_v32_cpu_width_evidence_t) == 8u,
               "V32 CPU evidence ABI changed");
#endif

#ifdef __cplusplus
}
#endif

#endif
