#ifndef RF_V20_VIDEO_POSTPROCESS_H
#define RF_V20_VIDEO_POSTPROCESS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RF_V20_INPUT_FREQUENCY_BINS 204u
#define RF_V20_INPUT_TIME_BINS 115u
#define RF_V20_INPUT_CHANNELS 4u
#define RF_V20_INPUT_BYTES 93840u

#define RF_V20_VIDEO_10MHZ_HZ INT32_C(10000000)
#define RF_V20_VIDEO_20MHZ_HZ INT32_C(20000000)

/* Raw V20 V3 INT8 logit thresholds after PTQ. */
#define RF_V20_VIDEO_HEATMAP_THRESHOLD_Q8 INT8_C(83)
#define RF_V20_VIDEO_SCORE_055_Q8 INT8_C(90)
#define RF_V20_VIDEO_SCORE_075_Q8 INT8_C(97)
#define RF_V20_VIDEO_SCORE_090_Q8 INT8_C(105)
#define RF_V20_VIDEO_DISPLAY_SCORE_Q8 INT32_C(-140)

typedef struct rf_v20_video_event {
    int32_t center_frequency_offset_hz;
    int32_t center_sample;
    int32_t bandwidth_hz;
} rf_v20_video_event_t;

typedef struct rf_v20_video_width_evidence {
    int16_t current_10mhz_contrast_codes;
    int16_t best_20mhz_contrast_codes;
    int16_t left_shoulder_contrast_codes;
    int16_t right_shoulder_contrast_codes;
    int8_t best_20mhz_shift_bins;
    uint8_t searched_center_count;
    uint8_t full_frequency_support;
    uint8_t base_upgrade_to_20mhz;
    uint8_t bilateral_upgrade_to_20mhz;
    uint8_t reserved[3];
} rf_v20_video_width_evidence_t;

typedef struct rf_v20_video_postprocess_result {
    rf_v20_video_event_t event;
    rf_v20_video_width_evidence_t width;
    int32_t width_shift_hz;
    int32_t frequency_bias_shift_hz;
    int16_t frequency_bias_q8;
    uint8_t width_upgraded;
    uint8_t reserved;
} rf_v20_video_postprocess_result_t;

/* Returns 1 when the V20 V3 raw heatmap logit may enter ROI processing. */
int rf_v20_video_heatmap_accepts(int8_t raw_logit);

/* Returns 0 below 0.55, then 1/2/3 for the existing SPRT score buckets. */
uint8_t rf_v20_video_score_tier(int8_t raw_logit);

/*
 * Applies V19 10/20 MHz energy-width logic, the V20 bilateral shoulder
 * repair, and the V20 fixed Q8 frequency-bias correction. The input event
 * center must already include V16 integer ROI and V17 sub-bin shifts.
 */
int rf_v20_video_postprocess(
    const int8_t *input_nhwc,
    size_t input_bytes,
    uint64_t capture_center_frequency_hz,
    const rf_v20_video_event_t *input_event,
    rf_v20_video_postprocess_result_t *result
);

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(rf_v20_video_event_t) == 12u,
               "V20 video event ABI changed");
#endif

#ifdef __cplusplus
}
#endif

#endif
