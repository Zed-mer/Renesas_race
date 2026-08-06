#ifndef RF_V16_ROI_POSTPROCESS_H
#define RF_V16_ROI_POSTPROCESS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RF_V16_CLASS_COUNT 5u
#define RF_V16_INPUT_FREQUENCY_BINS 204u
#define RF_V16_INPUT_TIME_BINS 115u
#define RF_V16_INPUT_CHANNELS 4u
#define RF_V16_INPUT_BYTES 93840u
#define RF_V16_Q8_ONE 256
#define RF_V16_Q15_ONE 32767u
#define RF_V16_ROI_PASS 1u
#define RF_V16_ROI_FAIL 2u

enum rf_v16_gate_kind {
    RF_V16_GATE_LINEAR = 0,
    RF_V16_GATE_COMPOSITE = 1
};

enum rf_v16_composite_flags {
    RF_V16_RULE_MIN_FREQUENCY_EDGE = 1u << 0,
    RF_V16_RULE_MAX_TEXTURE = 1u << 1,
    RF_V16_RULE_MAX_BURSTINESS = 1u << 2,
    RF_V16_RULE_MIN_CONTRAST = 1u << 3,
    RF_V16_RULE_MIN_OCCUPANCY = 1u << 4,
    RF_V16_RULE_MIN_VISIBLE_FRACTION = 1u << 5
};

typedef struct rf_v16_roi_statistics {
    int16_t contrast_q8;
    int16_t frequency_edge_q8;
    int16_t time_edge_q8;
    int16_t burstiness_q8;
    int16_t texture_q8;
    uint16_t occupancy_q15;
    uint16_t visible_fraction_q15;
} rf_v16_roi_statistics_t;

typedef struct rf_v16_linear_score_config {
    int16_t contrast_weight_q8;
    int16_t frequency_edge_weight_q8;
    int16_t time_edge_weight_q8;
    int16_t burstiness_weight_q8;
    int16_t occupancy_weight_q8;
    int16_t texture_weight_q8;
} rf_v16_linear_score_config_t;

typedef struct rf_v16_center_config {
    rf_v16_linear_score_config_t score;
    int16_t movement_penalty_q8;
    int16_t minimum_score_gain_q8;
    uint16_t minimum_visible_fraction_q15;
    uint8_t frequency_radius_bins;
    uint8_t time_radius_bins;
    uint8_t ring_frequency_bins;
    uint8_t ring_time_bins;
} rf_v16_center_config_t;

typedef struct rf_v16_composite_rule {
    int16_t minimum_frequency_edge_q8;
    int16_t maximum_texture_q8;
    int16_t maximum_burstiness_q8;
    int16_t minimum_contrast_q8;
    uint16_t minimum_occupancy_q15;
    uint16_t minimum_visible_fraction_q15;
    uint16_t enabled_flags;
    uint16_t reserved;
} rf_v16_composite_rule_t;

typedef struct rf_v16_gate_config {
    rf_v16_linear_score_config_t score;
    rf_v16_composite_rule_t rule;
    int32_t threshold_q8;
    uint8_t kind;
    uint8_t reserved[3];
} rf_v16_gate_config_t;

typedef struct rf_v16_class_config {
    rf_v16_center_config_t center;
    rf_v16_gate_config_t display_gate;
    rf_v16_gate_config_t state_gate;
} rf_v16_class_config_t;

/* The center is canonical and is supplied before clipping the fixed box. */
typedef struct rf_v16_candidate {
    int32_t center_frequency_offset_hz;
    int32_t center_sample;
    uint16_t confidence_q15;
    uint8_t class_id;
    uint8_t event_flags;
} rf_v16_candidate_t;

typedef struct rf_v16_postprocess_result {
    rf_v16_roi_statistics_t statistics;
    int32_t frequency_shift_hz;
    int32_t time_shift_samples;
    int32_t display_score_q8;
    int32_t state_score_q8;
    int8_t offset_frequency_bins;
    int8_t offset_time_bins;
    uint8_t display_accept;
    uint8_t state_roi_decision;
    int8_t subbin_frequency_steps;
    int8_t subbin_time_steps;
    uint8_t subbin_subdivisions;
    uint8_t subbin_method;
} rf_v16_postprocess_result_t;

typedef enum rf_v17_subbin_method {
    RF_V17_SUBBIN_DISABLED = 0,
    RF_V17_SUBBIN_LOCALIZATION = 1,
    RF_V17_SUBBIN_CONTRAST = 2
} rf_v17_subbin_method_t;

typedef struct rf_v17_subbin_config {
    uint8_t method;
    uint8_t subdivisions;
    uint8_t scale_numerator;
    uint8_t scale_denominator;
} rf_v17_subbin_config_t;

extern const rf_v16_class_config_t
    g_rf_v16_class_configs[RF_V16_CLASS_COUNT];

int32_t rf_v16_linear_score_q8(
    const rf_v16_roi_statistics_t *statistics,
    const rf_v16_linear_score_config_t *config
);

int rf_v16_composite_accepts(
    const rf_v16_roi_statistics_t *statistics,
    const rf_v16_composite_rule_t *rule
);

int rf_v16_postprocess_candidate(
    const int8_t *input_nhwc,
    size_t input_bytes,
    const rf_v16_candidate_t *candidate,
    const rf_v16_class_config_t *class_configs,
    rf_v16_postprocess_result_t *result
);

/* V17 keeps V16 gates and fixed geometry, then adds a bounded sub-bin shift. */
int rf_v17_postprocess_candidate(
    const int8_t *input_nhwc,
    size_t input_bytes,
    const rf_v16_candidate_t *candidate,
    const rf_v16_class_config_t *class_configs,
    const rf_v17_subbin_config_t *subbin_configs,
    rf_v16_postprocess_result_t *result
);

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(rf_v16_roi_statistics_t) == 14u,
               "V16 statistics layout changed");
_Static_assert(sizeof(rf_v16_candidate_t) == 12u,
               "V16 candidate layout changed");
#endif

#ifdef __cplusplus
}
#endif

#endif
